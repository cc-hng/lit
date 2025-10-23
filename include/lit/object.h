#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gsl/gsl>

namespace lit {

namespace detail {
template <int i>
using Int2Type = std::integral_constant<int, i>;
}

namespace net    = boost::asio;
namespace beast  = boost::beast;
namespace http   = beast::http;
using tcp        = net::ip::tcp;
using tcp_stream = typename beast::tcp_stream::rebind_executor<
    net::use_awaitable_t<>::executor_with_default<net::any_io_executor>>::other;
using next_handler = std::function<net::awaitable<void>()>;

class HttpStream;

struct request_t : public http::request<http::string_body> {
    mutable std::string_view path;
    mutable std::optional<std::unordered_map<std::string, std::string>> queries;
    mutable std::optional<std::unordered_map<std::string, std::string>> params;

    using base_type = http::request<http::string_body>;
    using base_type::base_type;

    request_t(base_type&& req) : base_type(std::move(req)) {}
    bool compile_target() const;
};

struct response_t : public http::response<http::buffer_body> {
private:
    enum { SMALL_CHUNK = 256 };
    bool is_streamed_;
    char buf_sm_[SMALL_CHUNK];
    char* buf_;
    int capacity_;

public:
    gsl::not_null<HttpStream*> stream;
    using base_type = http::response<http::buffer_body>;
    using base_type::base_type;

    response_t(HttpStream* s);
    ~response_t();

    bool is_streamed() const { return is_streamed_; }
    void streamed(bool s) { is_streamed_ = s; }

    void set_body(gsl::span<char> span, bool more = false);
    void set_body(std::string_view s, bool more = false);
    void text(std::string_view body, std::string_view ctype = "text/plain");
    void json(std::string_view body);

    void set_body_view(std::string_view s, bool more = false);
    void text_view(std::string_view body, std::string_view ctype = "text/plain");
    void json_view(std::string_view body);

private:
    void ensure_buffer(int n);
};

class HttpStream {
    void* stream_;
    std::function<net::awaitable<void>(request_t& req, int body_limit)> read_;
    std::function<net::awaitable<void>(const response_t&)> write_;
    std::function<net::awaitable<void>(
        http::response_serializer<typename response_t::body_type>&)>
        write_header_;
    std::function<net::awaitable<void>(
        http::response_serializer<typename response_t::body_type>&)>
        write_body_;

    std::function<void(int)> expires_after_;

public:
    template <typename Stream>
    HttpStream(Stream* stream, beast::flat_buffer& b) : stream_(stream) {
        read_ = [stream, &b](request_t& req, int body_limit) -> net::awaitable<void> {
            http::request_parser<typename request_t::body_type> request_parser;
            if (body_limit > 0) {
                request_parser.body_limit(body_limit);
            }
            co_await http::async_read(*stream, b, request_parser);
            req = {request_parser.release()};
        };

        write_ = [stream](const response_t& res) -> net::awaitable<void> {
            http::response_serializer<http::buffer_body> sr{res};
            co_await http::async_write(*stream, sr, net::use_awaitable);
        };

        write_header_ =
            [stream](http::response_serializer<typename response_t::body_type>& sr)
            -> net::awaitable<void> {
            co_await http::async_write_header(*stream, sr, net::use_awaitable);
        };

        write_body_ = [stream](http::response_serializer<typename response_t::body_type>& sr)
            -> net::awaitable<void> {
            co_await http::async_write(*stream, sr, net::use_awaitable);
        };

        expires_after_ = [stream](int seconds) {
            beast::get_lowest_layer(*stream).expires_after(
                std::chrono::seconds(seconds > 0 ? seconds : INT_MAX));
        };
    }

    net::awaitable<void> async_read(request_t& req, int body_limit = -1) {
        co_await read_(req, body_limit);
    }

    net::awaitable<void> async_write(const response_t& res) { co_await write_(res); }

    net::awaitable<void>
    async_write_header(http::response_serializer<typename response_t::body_type>& sr) {
        co_await write_header_(sr);
    }

    net::awaitable<void>
    async_write_body(http::response_serializer<typename response_t::body_type>& sr) {
        co_await write_body_(sr);
    }

    void expires_after(int seconds) { expires_after_(seconds); }

    void* native() const { return stream_; }
};

using route_handler =
    std::function<net::awaitable<void>(const request_t&, response_t&, const next_handler&)>;

}  // namespace lit
