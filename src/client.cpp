#include "lit/client.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#if LIT_USE_SSL
#    include <boost/asio/ssl.hpp>
#    include <boost/beast/ssl.hpp>
#endif
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/noncopyable.hpp>

namespace lit {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace detail {

using tcp_stream = typename beast::tcp_stream::rebind_executor<
    net::use_awaitable_t<>::executor_with_default<net::any_io_executor>>::other;
#if LIT_USE_SSL
using tls_stream  = net::ssl::stream<tcp_stream>;
using conn_stream = std::variant<tcp_stream, tls_stream>;
#else
using conn_stream = std::variant<tcp_stream>;
#endif

struct Connection {
#if LIT_USE_SSL
    std::shared_ptr<net::ssl::context> ctx;  // outlives stream (declared first)
#endif
    conn_stream stream;
    std::string key;      // "host:port:tcp" or "host:port:tls"
    bool        reused {};

    Connection(conn_stream&& s,
#if LIT_USE_SSL
               std::shared_ptr<net::ssl::context> c,
#endif
               std::string k)
#if LIT_USE_SSL
      : ctx(std::move(c)),
#endif
        stream(std::move(s)), key(std::move(k)) {}
    // tcp_stream / ssl::stream 析构自动关闭底层 socket

};

template <typename F>
net::awaitable<void> conn_write(Connection& c, F& msg) {
    co_await std::visit([&msg](auto& s) -> net::awaitable<void> {
        co_await http::async_write(s, msg);
    }, c.stream);
}
template <typename Res>
net::awaitable<void> conn_read(Connection& c, beast::flat_buffer& b, Res& res) {
    co_await std::visit([&](auto& s) -> net::awaitable<void> {
        co_await http::async_read(s, b, res);
    }, c.stream);
}

class ConnectionPool : boost::noncopyable {
public:
    static ConnectionPool& instance() {
        static ConnectionPool ins;
        return ins;
    }

    void cleanup() {
        std::unique_lock<std::mutex> lock(mtx_);
        connections_ = {};
    }

    net::awaitable<std::shared_ptr<Connection>>
    get_connection(std::string_view host, int port, bool tls, bool verify,
                   const std::string& ca) {
        const std::string key = make_key(host, port, tls);
        {
            std::unique_lock<std::mutex> lock(mtx_);
            auto it = connections_.find(key);
            if (it != connections_.end() && !it->second.empty()) {
                auto conn = it->second.front();
                it->second.pop_front();
                if (it->second.empty()) {
                    connections_.erase(it);
                }
                conn->reused = true;
                co_return conn;
            }
        }

        auto resolver = net::use_awaitable.as_default_on(
            tcp::resolver(co_await net::this_coro::executor));
        tcp_stream stream = net::use_awaitable.as_default_on(
            beast::tcp_stream(co_await net::this_coro::executor));

        auto const results = co_await resolver.async_resolve(host, std::to_string(port));
        co_await stream.async_connect(results);

#if LIT_USE_SSL
        if (tls) {
            auto ctx = std::make_shared<net::ssl::context>(net::ssl::context::tls_client);
            if (verify) {
                ctx->set_default_verify_paths();
                ctx->set_verify_mode(net::ssl::verify_peer);
                if (!ca.empty()) {
                    ctx->load_verify_file(ca);
                }
            } else {
                ctx->set_verify_mode(net::ssl::verify_none);
            }
            std::string h(host);
            net::ssl::stream<tcp_stream> ss{std::move(stream), *ctx};
            SSL_set_tlsext_host_name(ss.native_handle(), h.c_str());
            if (verify) {
                ss.set_verify_callback(net::ssl::host_name_verification(h));
            }
            co_await ss.async_handshake(net::ssl::stream_base::client, net::use_awaitable);
            co_return std::make_shared<Connection>(conn_stream{std::move(ss)}, ctx, key);
        }
#endif
#if LIT_USE_SSL
        co_return std::make_shared<Connection>(conn_stream{std::move(stream)}, nullptr, key);
#else
        co_return std::make_shared<Connection>(conn_stream{std::move(stream)}, key);
#endif
    }

    void save_connection(std::shared_ptr<Connection> conn) {
        std::unique_lock<std::mutex> lock(mtx_);
        auto& queue = connections_[conn->key];
        if (queue.size() < max_per_host_) {
            queue.push_back(std::move(conn));
        }
    }

private:
    static std::string make_key(std::string_view host, int port, bool tls) {
        return std::string(host) + ":" + std::to_string(port) + (tls ? ":tls" : ":tcp");
    }

    static constexpr std::size_t max_per_host_ = 128;
    std::mutex mtx_;
    std::unordered_map<std::string, std::deque<std::shared_ptr<Connection>>> connections_;
};
}  // namespace detail

template <typename ReqBody, typename RespBody>
net::awaitable<http::response<RespBody>>
fetch(std::string_view host, int port, http::request<ReqBody> req,
      const fetch_option_t& opt) {
    detail::ConnectionPool& pool = detail::ConnectionPool::instance();
    if (port <= 0 || port > 65535) {
        throw std::invalid_argument("invalid port");
    }
    if (opt.timeout <= 0) {
        throw std::invalid_argument("timeout must be positive");
    }
    req.keep_alive(opt.keepalive);
    req.prepare_payload();

    const int attempts = opt.max_retry > 0 ? opt.max_retry : 1;
    for (int retry = attempts; retry > 0; --retry) {
        std::shared_ptr<detail::Connection> conn;
        try {
            conn = co_await pool.get_connection(host, port, opt.tls, opt.verify, opt.ca);
            std::visit([&](auto& s) {
                if constexpr (std::is_same_v<std::decay_t<decltype(s)>, detail::tls_stream>)
                    s.next_layer().expires_after(std::chrono::seconds(opt.timeout));
                else
                    s.expires_after(std::chrono::seconds(opt.timeout));
            }, conn->stream);

            co_await detail::conn_write(*conn, req);

            http::response<RespBody> res;
            beast::flat_buffer buffer;
            co_await detail::conn_read(*conn, buffer, res);

            if (opt.keepalive && res.keep_alive()) {
                pool.save_connection(std::move(conn));
            }
            co_return res;
        } catch (...) {
            // Drop a failed reused connection and reconnect.
            if (conn && conn->reused && opt.keepalive && retry > 1) {
                continue;
            }
            throw;
        }
    }

    throw std::logic_error("unreachable");
}

// REST/JSON
template net::awaitable<http::response<http::string_body>>
fetch<http::string_body, http::string_body>(std::string_view, int,
        http::request<http::string_body>, const fetch_option_t&);

// Static download
template net::awaitable<http::response<http::file_body>>
fetch<http::empty_body, http::file_body>(std::string_view, int,
        http::request<http::empty_body>, const fetch_option_t&);

void http_fetch_pool_cleanup() {
    detail::ConnectionPool::instance().cleanup();
}

}  // namespace lit
