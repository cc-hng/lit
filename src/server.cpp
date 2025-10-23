#include "lit/server.h"
#include "lit/middleware.hpp"
#include <algorithm>
#include <sstream>
#include <vector>
#include <gsl/gsl>

#if LIT_WITH_OPENSSL
#    include <boost/asio/ssl.hpp>
#    include <boost/asio/ssl/error.hpp>
namespace ssl = boost::asio::ssl;
#endif

namespace {
inline constexpr auto _default_on_error = [](std::string_view) {
};

inline std::string fail(std::string_view what, std::string_view msg) {
    std::ostringstream oss;
    oss << what << ": " << msg;
    return oss.str();
}
}  // namespace

namespace lit {

App::App(net::io_context& ctx, const option_t& option)
  : ioc_(ctx)
  , option_(option)
  , on_error_(_default_on_error)
#if LIT_WITH_OPENSSL
  , ssl_ctx_(nullptr)
#endif
{
    use(lit::middleware::auto_headers);
}

void App::start(std::string_view ip, uint16_t port) {
    std::call_once(flag_, [&, this] {
        auto address = net::ip::make_address(ip);
        net::co_spawn(ioc_, do_listen(tcp::endpoint{address, port}), handle_exception);
    });
}

#if LIT_WITH_OPENSSL
void App::start(std::string_view ip, uint16_t port, tls_option_t tls) {
    std::call_once(flag_, [&, this] {
        ssl_ctx_ = std::make_shared<ssl::context>(ssl::context::method(tls.method));

        ssl_ctx_->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2
                              | ssl::context::single_dh_use);
        ssl_ctx_->use_certificate_chain(
            boost::asio::buffer(tls.cert.data(), tls.cert.size()));
        ssl_ctx_->use_private_key(boost::asio::buffer(tls.key.data(), tls.key.size()),
                                  ssl::context::file_format::pem);
        ssl_ctx_->use_tmp_dh(boost::asio::buffer(tls.dh.data(), tls.dh.size()));

        auto address = net::ip::make_address(ip);
        net::co_spawn(ioc_, do_listen(tcp::endpoint{address, port}, *ssl_ctx_),
                      handle_exception);
    });
}

net::awaitable<void> App::do_listen(tcp::endpoint endpoint, net::ssl::context& ctx) {
    // Open the acceptor
    auto acceptor =
        net::use_awaitable.as_default_on(tcp::acceptor(co_await net::this_coro::executor));
    acceptor.open(endpoint.protocol());

    // Disallow address reuse
    acceptor.set_option(net::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(endpoint);

    // Start listening for connections
    // const auto backlog = net::socket_base::max_listen_connections;
    const auto backlog = option_.backlog;
    acceptor.listen(backlog);

    for (;;) {
        auto stream =
            std::make_shared<ssl::stream<tcp_stream>>(co_await acceptor.async_accept(),
                                                      ctx);

        net::co_spawn(
            acceptor.get_executor(),
            [this, stream]() -> net::awaitable<void> {
                auto& lowest_layer = beast::get_lowest_layer(*stream);

                lowest_layer.expires_after(
                    std::chrono::seconds(option_.timeout > 0 ? option_.timeout : INT_MAX));
                co_await stream->async_handshake(ssl::stream_base::server,
                                                 net::use_awaitable);
                co_await do_session(stream.get());
            },
            [this](auto e) {
                try {
                    handle_exception(e);
                } catch (boost::system::system_error& e0) {
                    auto code = e0.code();
                    on_error_(fail("accept", code.message()));
                } catch (std::exception& e0) {
                    throw;
                }
            });
    }
}
#endif

App& App::serve_static(std::string_view mountpoint, std::string_view dir) {
    return use(middleware::StaticFileProvider(mountpoint, dir));
}

void App::handle_exception(std::exception_ptr e) {
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

// Accepts incoming connections and launches the sessions
net::awaitable<void> App::do_listen(tcp::endpoint endpoint) {
    // Open the acceptor
    auto acceptor =
        net::use_awaitable.as_default_on(tcp::acceptor(co_await net::this_coro::executor));
    acceptor.open(endpoint.protocol());

    // Disallow address reuse
    acceptor.set_option(net::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(endpoint);

    // Start listening for connections
    // const auto backlog = net::socket_base::max_listen_connections;
    const auto backlog = option_.backlog;
    acceptor.listen(backlog);

    for (;;) {
        auto stream = std::make_shared<tcp_stream>(co_await acceptor.async_accept());
        net::co_spawn(
            acceptor.get_executor(),
            [this, stream]() -> net::awaitable<void> { co_await do_session(stream.get()); },
            [this](auto e) {
                try {
                    handle_exception(e);
                } catch (boost::system::system_error& e0) {
                    auto code = e0.code();
                    on_error_(fail("session", code.message()));
                } catch (std::exception& e0) {
                    throw;
                }
            });
    }
}

}  // namespace lit
