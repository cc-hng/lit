#include "lit/server.h"
#include "lit/middleware.hpp"
#include <algorithm>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/recycling_allocator.hpp>
#include <boost/beast.hpp>
#include <boost/beast/version.hpp>
#include <gsl/gsl>

namespace {
// Body sizes at or below this are copied into the header scratch buffer so a
// single write syscall serves the whole response; larger bodies are written
// separately to avoid the copy. Balance point is roughly 64-128KB on modern
// x86 (one extra composed write ~= memcpy of that size), so 64KB keeps every
// small/medium response on the single-syscall path and bounds the scratch
// buffer per connection.
constexpr std::size_t kFastPathCopyLimit = 65536;
}  // namespace

#if LIT_USE_SSL
#    include <boost/asio/ssl.hpp>
#    include <boost/asio/ssl/error.hpp>
namespace ssl = boost::asio::ssl;
#endif

namespace lit {

template <bool SSL>
TemplatedApp<SSL>::TemplatedApp(net::io_context& ctx, const option_t& opt)
  : ioc_(ctx)
  , opt_(opt) {
}

template <bool SSL>
void TemplatedApp<SSL>::start(std::string_view ip, uint16_t port) {
    if constexpr (SSL) {
        auto ctx = std::make_shared<ssl::context>(ssl::context::method(opt_.ssl_method));

        ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2
                         | ssl::context::single_dh_use);
        ctx->use_certificate_chain(
            boost::asio::buffer(opt_.ssl_cert.data(), opt_.ssl_cert.size()));
        ctx->use_private_key(boost::asio::buffer(opt_.ssl_key.data(), opt_.ssl_key.size()),
                             ssl::context::file_format::pem);
        ctx->use_tmp_dh(boost::asio::buffer(opt_.ssl_dh.data(), opt_.ssl_dh.size()));

        auto address            = net::ip::make_address(ip);
        session_starter starter = [this, ctx](tcp::socket sock) -> net::awaitable<void> {
            auto stream = std::make_shared<ssl::stream<tcp_stream>>(std::move(sock), *ctx);
            auto& lowest_layer = beast::get_lowest_layer(*stream);
            lowest_layer.expires_after(
                std::chrono::seconds(opt_.timeout > 0 ? opt_.timeout : INT_MAX));
            co_await stream->async_handshake(ssl::stream_base::server, net::use_awaitable);
            co_await do_session(stream.get());
        };
        net::co_spawn(ioc_, do_listen(tcp::endpoint{address, port}, std::move(starter)),
                      handle_exception);
    } else {
        auto address            = net::ip::make_address(ip);
        session_starter starter = [this](tcp::socket sock) -> net::awaitable<void> {
            auto stream = std::make_shared<tcp_stream>(std::move(sock));
            co_await do_session(stream.get());
        };
        net::co_spawn(ioc_, do_listen(tcp::endpoint{address, port}, std::move(starter)),
                      handle_exception);
    }
}

template <bool SSL>
void TemplatedApp<SSL>::handle_exception(std::exception_ptr e) {
    if (!e) return;

    try {
        std::rethrow_exception(e);
    } catch (boost::system::system_error& e) {
        bool ignore = e.code() == net::error::connection_reset
                      || e.code() == net::error::timed_out
                      || e.code() == boost::beast::error::timeout;
        if (!ignore) {
            throw;
        }
    } catch (std::exception& e) {
        throw;
    }
}

// Accepts incoming connections and hands each to the injected session starter
// (plain or TLS adapter built by start()).
template <bool SSL>
net::awaitable<void>
TemplatedApp<SSL>::do_listen(tcp::endpoint endpoint, session_starter starter) {
    // Open the acceptor
    auto acceptor =
        net::use_awaitable.as_default_on(tcp::acceptor(co_await net::this_coro::executor));
    acceptor.open(endpoint.protocol());

    // Disallow address reuse
    acceptor.set_option(net::socket_base::reuse_address(true));

#ifdef SO_REUSEPORT
    // Multi-thread model: several Apps (one per io_context) bind the same
    // port; the kernel round-robins connections (uWS/nginx style). Needs a
    // native setsockopt -- asio has no reuse_port option. Windows lacks
    // SO_REUSEPORT: there each App must use a distinct port.
    int one = 1;
    ::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    // Bind to the server address
    acceptor.bind(endpoint);

    // Start listening for connections
    // const auto backlog = net::socket_base::max_listen_connections;
    const auto backlog = opt_.backlog;
    acceptor.listen(backlog);

    // Non-blocking so the accept path can drain the pending queue in one
    // event-loop turn instead of one accept per epoll wakeup.
    acceptor.non_blocking(true);

    for (;;) {
        auto sock = co_await acceptor.async_accept();
        start_session(starter, acceptor.get_executor(), std::move(sock));
        // Drain: accept everything already queued this turn.
        for (;;) {
            tcp::socket next(acceptor.get_executor());
            beast::error_code ec;
            acceptor.accept(next, ec);
            if (ec == net::error::would_block) {
                break;
            }
            if (ec) {
                // transient accept errors (e.g. connection reset) are ignored;
                // treat anything else as end-of-drain
                if (ec != net::error::connection_reset
                    && ec != net::error::connection_aborted) {
                    break;
                }
                continue;
            }
            start_session(starter, acceptor.get_executor(), std::move(next));
        }
    }
}

// Start one accepted connection: its coroutine inherits the acceptor's
// executor, so the whole connection stays on this context/thread. The
// per-connection coroutine frame is the hot allocation path; route it through
// asio's thread-local recycling allocator instead of malloc.
template <bool SSL>
void TemplatedApp<SSL>::start_session(session_starter& starter,
                                      net::any_io_executor executor,
                                      tcp::socket sock) {
    net::co_spawn(std::move(executor), starter(std::move(sock)),
                  net::bind_allocator(net::recycling_allocator<void>(), [](auto e) {
                      try {
                          handle_exception(e);
                      } catch (boost::system::system_error& e0) {
                          // auto code = e0.code();
                          // on_error_(fail("session", code.message()));
                      } catch (std::exception& e0) {
                          throw;
                      }
                  }));
}

template <bool SSL>
net::awaitable<void> TemplatedApp<SSL>::do_session(stream_type* stream) {
    // TCP_NODELAY: the fast path issues header/body as two small writes; Nagle
    // would coalesce them into a delayed segment. All HTTP servers disable it.
    {
        beast::error_code ec;
        beast::get_lowest_layer(*stream).socket().set_option(net::ip::tcp::no_delay(true),
                                                             ec);
    }
    beast::flat_buffer buffer;
    // Preallocate: response headers + start line fit within the request
    // header budget; avoids reallocation and keeps the common path alloc-free
    // after the first request.
    std::string scratch;
    scratch.reserve(kMaxHttpHeaders);
    alloc_t alloc(kMaxHttpHeaders);

    auto _defer = gsl::finally([&] {
        // Send a TCP shutdown
        if (stream) [[likely]] {
            beast::error_code ec;
            std::ignore = beast::get_lowest_layer(*stream).socket().shutdown(
                tcp::socket::shutdown_send, ec);
        }
    });

    try {
        for (;;) {
            beast::get_lowest_layer(*stream).expires_after(
                std::chrono::seconds(opt_.timeout > 0 ? opt_.timeout : INT_MAX));

            response_t res{stream, alloc};

            // read
            do {
                http::request_parser<typename request_t::body_type, alloc_t> request_parser{
                    std::piecewise_construct, std::make_tuple(), std::make_tuple(alloc)};
                if (opt_.body_limit > 0) {
                    request_parser.body_limit(opt_.body_limit);
                }
                co_await http::async_read(*stream, buffer, request_parser);
                request_t req{request_parser.release()};

                res.result(http::status::not_found);
                res.version(req.version());
                res.keep_alive(req.keep_alive());
                res.set(http::field::server, BOOST_BEAST_VERSION_STRING);

                co_await router_.handle(req, res);

                if (res.result() == http::status::not_found) {
                    res.set_body_view("Not found\n");
                } else if (res.body().data == nullptr) {
                    res.set_body_view("");
                }
                if (res.find(http::field::content_type) == res.end()) {
                    res.set(http::field::content_type, "text/plain");
                }
            } while (0);

            bool keep_alive = false;
            if (GSL_LIKELY(!res.is_streamed())) {
                keep_alive = res.keep_alive();
                // Fast path: build the complete wire bytes in one contiguous
                // buffer and issue a single composed write. Skips the
                // serializer's per-byte buffer-chain machinery (buffers_cat /
                // buffers_suffix / variant dispatch) that dominates the
                // profile of small responses. Falls back to the Beast
                // serializer for chunked/HTTP-1.0/missing-length cases.
                if (!res.chunked() && res.body().more == false && res.version() >= 11)
                    [[likely]] {
                    scratch.clear();
                    // Status line lookup: the most common codes are appended
                    // in one shot (no itoa, no reason lookup). Anything else
                    // falls back to itoa + Beast's reason table.
                    switch (res.result_int()) {
                        case 200: scratch.append("HTTP/1.1 200 OK\r\n"); break;
                        case 201: scratch.append("HTTP/1.1 201 Created\r\n"); break;
                        case 204: scratch.append("HTTP/1.1 204 No Content\r\n"); break;
                        case 301: scratch.append("HTTP/1.1 301 Moved Permanently\r\n"); break;
                        case 302: scratch.append("HTTP/1.1 302 Found\r\n"); break;
                        case 304: scratch.append("HTTP/1.1 304 Not Modified\r\n"); break;
                        case 400: scratch.append("HTTP/1.1 400 Bad Request\r\n"); break;
                        case 401: scratch.append("HTTP/1.1 401 Unauthorized\r\n"); break;
                        case 403: scratch.append("HTTP/1.1 403 Forbidden\r\n"); break;
                        case 404: scratch.append("HTTP/1.1 404 Not Found\r\n"); break;
                        case 500: scratch.append("HTTP/1.1 500 Internal Server Error\r\n"); break;
                        case 503: scratch.append("HTTP/1.1 503 Service Unavailable\r\n"); break;
                        default: {
                            scratch.append("HTTP/1.1 ");
                            {
                                char tmp[16];
                                int len = 0;
                                int v = res.result_int();
                                do {
                                    tmp[len++] = char('0' + v % 10);
                                    v /= 10;
                                } while (v);
                                while (len) {
                                    scratch.push_back(tmp[--len]);
                                }
                            }
                            scratch.push_back(' ');
                            scratch.append(http::obsolete_reason(res.result()).data(),
                                          http::obsolete_reason(res.result()).size());
                            scratch.append("\r\n");
                        }
                    }
                    for (auto const& f : res.base()) {
                        scratch.append(f.name_string().data(), f.name_string().size());
                        scratch.append(": ", 2);
                        scratch.append(f.value().data(), f.value().size());
                        scratch.append("\r\n", 2);
                    }
                    scratch.append("\r\n", 2);
                    // Small body: flatten into scratch and issue one write --
                    // a single syscall beats two composed writes for tiny
                    // payloads. Large body: write headers from scratch, then
                    // the body straight from the source buffer (no copy).
                    const std::size_t body_size =
                        res.body().data != nullptr ? res.body().size : 0;
                    if (body_size <= kFastPathCopyLimit) {
                        if (body_size > 0) {
                            scratch.append(static_cast<const char*>(res.body().data),
                                           body_size);
                        }
                        co_await net::async_write(*stream, net::buffer(scratch),
                                                  net::use_awaitable);
                    } else {
                        co_await net::async_write(*stream, net::buffer(scratch),
                                                  net::use_awaitable);
                        co_await net::async_write(*stream,
                                                  net::buffer(static_cast<const char*>(
                                                                  res.body().data),
                                                              body_size),
                                                  net::use_awaitable);
                    }
                } else {
                    http::response_serializer<http::buffer_body, http::basic_fields<alloc_t>> sr{
                        res};
                    co_await http::async_write(*stream, sr, net::use_awaitable);
                }
            }

            if (!keep_alive) {
                break;
            }
        }
    } catch (boost::system::system_error& se) {
        auto code = se.code();
        // websocket handshake/upgrade failures are per-session outcomes,
        // not server bugs -- swallow them so a plain HTTP GET that hits a
        // websocket route (upgrade_declined) closes cleanly instead of
        // terminating the process via the completion handler rethrow.
        const bool ws_handshake_failed = code == beast::websocket::condition::handshake_failed;
        if (code != http::error::end_of_stream && code != net::error::connection_reset
            && code != net::error::broken_pipe && code != net::error::timed_out
            && code != net::error::operation_aborted && !ws_handshake_failed) {
            throw;
        }
    }
}

template <bool SSL>
TemplatedApp<SSL>&
TemplatedApp<SSL>::serve_static(std::string_view mountpoint, std::string_view dir) {
    return use(middleware::StaticFileProvider(mountpoint, dir));
}

template <bool SSL>
void TemplatedApp<SSL>::stream_expires_after(stream_type* stream, int seconds) {
    beast::get_lowest_layer(*stream).expires_after(
        std::chrono::seconds(seconds > 0 ? seconds : INT_MAX));
}

}  // namespace lit

namespace lit {
// Explicit instantiation: definitions stay in this TU, one per TLS setting.
template class TemplatedApp<0>;
#if LIT_USE_SSL
template class TemplatedApp<1>;
#endif
}  // namespace lit
