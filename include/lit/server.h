#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/noncopyable.hpp>
#include <lit/detail/move_only_function.h>
#include <lit/object.h>
#include <lit/router.h>
#include <stdint.h>

#if LIT_USE_SSL
#    include <boost/asio/ssl.hpp>
#    include <boost/beast/ssl.hpp>
#endif

namespace lit {

template <bool SSL>
class TemplatedApp : boost::noncopyable {
    using session_starter = MoveOnlyFunction<net::awaitable<void>(tcp::socket)>;

public:
    struct option_t {
        int body_limit = -1;
        int timeout    = 30;    // seconds
        int backlog    = 1024;  // accept queue; kernel caps at somaxconn
#if LIT_USE_SSL
        int ssl_method       = net::ssl::context::tlsv12;
        std::string ssl_cert = "";
        std::string ssl_key  = "";
        std::string ssl_dh   = "";
#endif
    };

#if LIT_USE_SSL
    using stream_type = std::conditional_t<SSL, net::ssl::stream<tcp_stream>, tcp_stream>;
#else
    // static_assert(!SSL, "!!! Required build with `LIT_USE_SSL' !!!");
    // fallback to nossl
    using stream_type = tcp_stream;
#endif

private:
    net::io_context& ioc_;
    const option_t opt_;
    Router router_;

public:
    TemplatedApp(net::io_context& ctx, const option_t& opt = {});
    ~TemplatedApp() = default;

    void start(std::string_view ip, uint16_t port);

    template <typename H>
    inline TemplatedApp& use(H&& h) {
        router_.use(std::forward<H>(h));
        return *this;
    }

    template <typename H>
    inline TemplatedApp& route(http::verb method, std::string_view path, H&& h) {
        router_.route(method, path, std::forward<H>(h));
        return *this;
    }

    template <typename H>
    inline TemplatedApp& Get(std::string_view path, H&& h) {
        return route(http::verb::get, path, std::forward<H>(h));
    }

    template <typename H>
    inline TemplatedApp& Post(std::string_view path, H&& h) {
        return route(http::verb::post, path, std::forward<H>(h));
    }

    template <typename H>
    inline TemplatedApp& Any(std::string_view path, H&& h) {
        return route((http::verb)-1, path, std::forward<H>(h));
    }

    inline auto group(std::string_view prefix) { return router_.group(prefix); }

    // TODO: wire real per-session error reporting (see start_session in
    // server.cpp for the intended hook-up); currently a no-op stub, so the
    // callback registered here is never invoked.
    template <typename H>
    void on_error(H&& h) {}

    TemplatedApp& serve_static(std::string_view mountpoint, std::string_view dir);

    template <typename H>
    inline TemplatedApp& stream(std::string_view path, H&& h) {
        router_.route((http::verb)-1, path,
                      [h = std::forward<H>(h)](const request_t& req,  //
                                               response_t& res, const next_handler& go)
                          -> net::awaitable<void> {
                          stream_expires_after((stream_type*)res.stream, -1);
                          res.keep_alive(true);
                          res.streamed(true);
                          co_await h(req, res, go);
                      });
        return *this;
    }

    // Defined in lit/websocket.h; only consumers that use websockets pull it
    // in, so a plain HTTP server never drags in Boost.Beast websocket.
    template <typename H>
    TemplatedApp& websocket(std::string_view path, H&& h);

private:
    static void handle_exception(std::exception_ptr e);
    static void stream_expires_after(stream_type* stream, int seconds);

    // starter by value: a coroutine frame copies its parameters, so a
    // reference to a start()-local std::function would dangle.
    net::awaitable<void> do_listen(tcp::endpoint endpoint, session_starter starter);
    void start_session(session_starter& starter, net::any_io_executor executor,
                       tcp::socket sock);
    net::awaitable<void> do_session(stream_type* stream);
};

using App = TemplatedApp<0>;
#if LIT_USE_SSL
using SSLApp = TemplatedApp<1>;
#endif

}  // namespace lit
