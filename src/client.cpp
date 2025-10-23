#include "lit/client.h"
#include "detail/url.h"
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/noncopyable.hpp>
#include <gsl/gsl>

namespace lit {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = boost::asio::ip::tcp;
using namespace std::chrono_literals;

namespace detail {

using tcp_stream = typename beast::tcp_stream::rebind_executor<
    net::use_awaitable_t<>::executor_with_default<net::any_io_executor>>::other;

struct Connection {
    tcp_stream stream;
    std::string host;
    std::string port;
    bool tls;
    bool first;

    Connection(tcp_stream&& s, std::string h, std::string p, bool t, bool f = true)
      : stream(std::move(s))
      , host(std::move(h))
      , port(std::move(p))
      , tls(t)
      , first(f) {}

    ~Connection() {
        // Gracefully close the socket
        if (stream.socket().is_open()) {
            beast::error_code ec;
            std::ignore = stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        }
    }
};

class ConnectionPool : boost::noncopyable {
public:
    static ConnectionPool& instance() {
        static ConnectionPool ins;
        return ins;
    }

    ~ConnectionPool() = default;

    void cleanup() {
        std::unique_lock<std::mutex> lock(mtx_);
        connections_ = {};
    }

    net::awaitable<std::shared_ptr<Connection>>
    get_connection(const std::string& host, const std::string& port, bool tls) {
        std::unique_lock<std::mutex> lock(mtx_);
        std::string key = host + ":" + port;
        auto& queue     = connections_[key];

        if (!queue.empty()) {
            auto conn = queue.front();
            queue.pop_front();
            co_return conn;
        }

        lock.unlock();
        auto resolver = net::use_awaitable.as_default_on(
            tcp::resolver(co_await net::this_coro::executor));
        tcp_stream stream = net::use_awaitable.as_default_on(
            beast::tcp_stream(co_await net::this_coro::executor));

        auto const results = co_await resolver.async_resolve(host, port);
        co_await stream.async_connect(results);

        if (tls) {
            // 处理 TLS 连接初始化（如有需要）
        }

        co_return std::make_shared<Connection>(std::move(stream), host, port, tls);
    }

    void save_connection(std::shared_ptr<Connection> conn) {
        std::unique_lock<std::mutex> lock(mtx_);
        std::string key = conn->host + ":" + conn->port;
        auto& queue     = connections_[key];
        if (queue.size() < max_per_host_) {
            conn->first = false;
            queue.push_back(conn);
        }
    }

private:
    const int max_per_host_ = 128;
    std::mutex mtx_;
    std::unordered_map<std::string, std::deque<std::shared_ptr<Connection>>> connections_;
};
}  // namespace detail

template <typename Body = beast::http::string_body>
net::awaitable<beast::http::response<Body>>
fetch(std::string_view url, const fetch_option_t options = {}) {
    detail::ConnectionPool& pool = detail::ConnectionPool::instance();
    url_t u;
    auto ok = url_parse(url, u);
    if (GSL_UNLIKELY(!ok || u.port == u.nport)) {
        throw std::runtime_error("invalid url");
    }
    if (GSL_UNLIKELY(u.path.empty())) {
        u.requri = "/" + u.requri;
        u.path   = "/";
    }

    for (int retry = options.max_retry; retry > 0; retry--) {
        std::shared_ptr<detail::Connection> conn;
        try {
            conn = co_await pool.get_connection(u.hostname, std::to_string(u.port),
                                                u.scheme == "https" ? true : false);

            http::request<http::string_body> req{options.method, u.requri, 11};
            req.set(http::field::host, u.host);
            req.set(http::field::content_type, "application/json");
            req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            req.keep_alive(options.keepalive);
            req.body() = std::move(options.body);
            for (const auto& kv : options.headers) {
                req.set(kv.name_string(), kv.value());
            }
            req.prepare_payload();

            conn->stream.expires_after(std::chrono::seconds(options.timeout));
            std::ignore = co_await http::async_write(conn->stream, req);

            http::response<Body> res;
            beast::flat_buffer buffer;
            std::ignore = co_await http::async_read(conn->stream, buffer, res);

            if (options.keepalive) {
                pool.save_connection(conn);
            }
            co_return res;
        } catch (...) {
            // 如果连接是keepalive的且抛异常(区分异常)
            // 则尝试新建连接或者从pool取新的连接，重新尝试
            if (conn && !conn->first && options.keepalive && retry > 0) {
                continue;
            } else {
                throw;
            }
        }
    }

    assert(0);
    http::response<Body> res;
    co_return res;
}

net::awaitable<http::response<http::string_body>>
http_get(std::string_view url, bool keepalive, const fetch_option_t::headers_type& headers) {
    fetch_option_t opt = {
        .method    = fetch_option_t::method_type::get,
        .headers   = headers,
        .keepalive = keepalive,
    };
    co_return co_await fetch(url, opt);
}

net::awaitable<http::response<http::string_body>>
http_post(std::string_view url, std::string body, bool keepalive,
          const fetch_option_t::headers_type& headers) {
    fetch_option_t opt = {
        .method    = fetch_option_t::method_type::post,
        .headers   = headers,
        .body      = std::move(body),
        .keepalive = keepalive,
    };
    co_return co_await fetch(url, opt);
}

net::awaitable<http::response<http::string_body>>
http_upload(std::string_view url, const std::vector<multipart_formdata_t>& formdata,
            bool keepalive, const fetch_option_t::headers_type& headers) {
    static constexpr auto generate_boundary = []() {
        static const std::string chars = "0123456789"
                                         "abcdefghijklmnopqrstuvwxyz"
                                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> dist(0, chars.size() - 1);

        std::string boundary = "----WebKitFormBoundary";
        for (int i = 0; i < 16; ++i) {  // Generate a 16-character random string
            boundary += chars[dist(generator)];
        }
        return boundary;
    };

    auto boundary = generate_boundary();
    auto body     = multipart_formdata_t::encode(formdata, boundary);
    auto headers0 = headers;
    headers0.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
    co_return co_await http_post(url, std::move(body), keepalive, headers0);
}

void http_fetch_pool_cleanup() {
    detail::ConnectionPool::instance().cleanup();
}

}  // namespace lit
