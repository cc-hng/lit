#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <boost/noncopyable.hpp>
#include <lit/object.h>
#include <lit/router.h>
#include <stdint.h>

#if LIT_WITH_OPENSSL
#    include <boost/asio/ssl.hpp>
#    include <boost/beast/ssl.hpp>
#endif

namespace lit {

class App final : boost::noncopyable {
public:
    struct option_t {
        int body_limit;
        int timeout;
        int backlog;
    };

#if LIT_WITH_OPENSSL
    struct tls_option_t {
        int method;
        std::string cert;
        std::string key;
        std::string dh;
    };
#endif

private:
    net::io_context& ioc_;
    const option_t option_;
    Router router_;
    std::once_flag flag_;
    std::function<void(std::string_view)> on_error_;
#if LIT_WITH_OPENSSL
    std::shared_ptr<net::ssl::context> ssl_ctx_;
#endif

public:
    App(net::io_context& ctx, const option_t& option = {-1, 30, 128});
    ~App() = default;

    void start(std::string_view ip, uint16_t port);

#if LIT_WITH_OPENSSL
    void start(std::string_view ip, uint16_t port, tls_option_t tls);
#endif

    template <typename H>
    void on_error(H&& h) {
        on_error_ = std::forward<H>(h);
    }

    App& serve_static(std::string_view mountpoint, std::string_view dir);

    template <typename H>
    inline App& websocket(std::string_view path, H&& h) {
        router_.route((http::verb)-1, path,
                      [this, h0 = std::forward<H>(h)](
                          const request_t& req, response_t& res,
                          const next_handler& go) mutable -> net::awaitable<void> {
                          res.keep_alive(true);
                          res.streamed(true);
                          auto* s = res.stream->native();
#if LIT_WITH_OPENSSL
                          if (ssl_ctx_) {
                              co_await websocket_impl(s, req, std::forward<H>(h0),
                                                      detail::Int2Type<1>{});
                          } else {
                              co_await websocket_impl(s, req, std::forward<H>(h0),
                                                      detail::Int2Type<0>{});
                          }

#else
                          co_await websocket_impl(s, req, std::forward<H>(h0),
                                                  detail::Int2Type<0>{});
#endif
                      });
        return *this;
    }

    template <typename H>
    inline App& stream(std::string_view path, H&& h) {
        router_.route((http::verb)-1, path,
                      [h = std::forward<H>(h),
                       this](const request_t& req, response_t& res,
                             const next_handler& go) -> net::awaitable<void> {
                          res.stream->expires_after(-1);
                          res.keep_alive(true);
                          res.streamed(true);
                          co_await h(req, res, go);
                      });
        return *this;
    }

    template <typename H>
    inline App& use(H&& h) {
        router_.use(std::forward<H>(h));
        return *this;
    }

    template <typename H>
    inline App& route(http::verb method, std::string_view path, H&& h) {
        router_.route(method, path, std::forward<H>(h));
        return *this;
    }

    template <typename H>
    inline App& Get(std::string_view path, H&& h) {
        return route(http::verb::get, path, std::forward<H>(h));
    }

    template <typename H>
    inline App& Post(std::string_view path, H&& h) {
        return route(http::verb::post, path, std::forward<H>(h));
    }

    template <typename H>
    inline App& Any(std::string_view path, H&& h) {
        return route((http::verb)-1, path, std::forward<H>(h));
    }

    inline auto group(std::string_view prefix) { return router_.group(prefix); }

private:
    template <typename Handler>
    net::awaitable<void>  //
    websocket_impl(void* stream, const request_t& req, Handler&& h, detail::Int2Type<0>) {
        auto p = static_cast<tcp_stream*>(stream);
        beast::websocket::stream<tcp_stream> ws{p->release_socket()};
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(
            beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
        co_await ws.async_accept(req);
        co_await std::forward<Handler>(h)(req, std::move(ws));
    }

#if LIT_WITH_OPENSSL
    template <typename Handler>
    net::awaitable<void>  //
    websocket_impl(void* stream, const request_t& req, Handler&& h, detail::Int2Type<1>) {
        auto* p = static_cast<net::ssl::stream<tcp_stream>*>(stream);
        beast::websocket::stream<net::ssl::stream<tcp_stream>> ws{std::move(*p)};
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(
            beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
        co_await ws.async_accept(req);
        co_await std::forward<Handler>(h)(req, std::move(ws));
    }
#endif

private:
    static void handle_exception(std::exception_ptr e);

    // Accepts incoming connections and launches the sessions
    net::awaitable<void> do_listen(tcp::endpoint endpoint);

#if LIT_WITH_OPENSSL
    net::awaitable<void> do_listen(tcp::endpoint endpoint, net::ssl::context& ssl_ctx);
#endif

    template <typename Stream>
    net::awaitable<void>  //
    do_session(Stream* stream) {
        beast::flat_buffer buffer;
        HttpStream http_stream(stream, buffer);

        auto _defer = gsl::finally([&] {
            // Send a TCP shutdown
            if (GSL_LIKELY(stream)) {
                beast::error_code ec;
                std::ignore = beast::get_lowest_layer(*stream).socket().shutdown(
                    tcp::socket::shutdown_send, ec);
            }
        });

        try {
            for (;;) {
                http_stream.expires_after(option_.timeout);

                request_t req;
                response_t res{&http_stream};

                co_await http_stream.async_read(req, option_.body_limit);
                co_await router_(req, res);

                bool keep_alive = false;
                if (GSL_LIKELY(!res.is_streamed())) {
                    keep_alive = res.keep_alive();
                    co_await http_stream.async_write(res);
                }

                if (!keep_alive) {
                    break;
                }
            }
        } catch (boost::system::system_error& se) {
            auto code = se.code();
            if (code != http::error::end_of_stream && code != net::error::connection_reset
                && code != net::error::broken_pipe && code != net::error::timed_out
                && code != net::error::operation_aborted
                && code != beast::websocket::error::closed) {
                throw;
            }
        }
    }
};

}  // namespace lit
