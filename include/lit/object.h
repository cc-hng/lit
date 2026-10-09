#pragma once

#include <optional>
#include <span>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>
#include <lit/detail/fields_alloc.hpp>
#include <lit/detail/move_only_function.h>

#ifndef LIT_MAX_HTTP_HEADERS
#    define LIT_MAX_HTTP_HEADERS 8192
#endif

namespace lit {

static constexpr std::size_t kMaxHttpHeaders = LIT_MAX_HTTP_HEADERS;

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = net::ip::tcp;

struct request_t;
struct response_t;

using tcp_stream   = typename beast::tcp_stream::rebind_executor<net::use_awaitable_t<
    net::any_io_executor>::executor_with_default<net::any_io_executor>>::other;
using next_handler = MoveOnlyFunction<net::awaitable<void>() const>;
using route_handler =
    MoveOnlyFunction<net::awaitable<void>(const request_t&, response_t&, const next_handler&)>;

using alloc_t = fields_alloc<char>;

template <typename K, typename V, size_t N = 4>
using small_flat_map =
    boost::container::flat_map<K, V, std::less<>,
                               boost::container::small_vector<std::pair<K, V>, N>>;

using route_parameters =
    boost::container::small_vector<std::pair<std::string, std::string>, 4>;
struct request_t : public http::request<http::string_body, http::basic_fields<alloc_t>> {
private:
    friend class Router;
    mutable size_t query_separator_ = std::string_view::npos;
    mutable bool target_compiled_   = false;
    mutable bool target_valid_      = true;
    mutable small_flat_map<std::string_view, std::string_view> queries_;
    mutable route_parameters parameters_;

    static std::string url_decode(std::string_view sv);

public:
    using base_type = http::request<http::string_body, http::basic_fields<alloc_t>>;
    using base_type::base_type;

    request_t(base_type&& req) : base_type(std::move(req)) {}
    bool compile_target() const;

    // --- 从 target() 解析 path（需先调用 compile_target）---
    std::string_view get_url() const;
    std::string get_query(std::string_view key) const;

    // --- 当前路由的路径参数 ---
    std::string_view get_parameter(std::string_view key) const;
    std::string_view get_parameter(size_t index) const;
};

struct response_t : public http::response<http::buffer_body, http::basic_fields<alloc_t>> {
private:
    bool is_streamed_;
    std::shared_ptr<std::string> owned_body_;
    boost::container::small_vector<char, 256> buffer_;

public:
    template <bool>
    friend class TemplatedApp;

    void* stream;
    using base_type = http::response<http::buffer_body, http::basic_fields<alloc_t>>;
    using base_type::base_type;

    response_t(void* s, alloc_t const& alloc);
    ~response_t();

    // Body-only setters, do not touch result: use these for custom status codes (206/416/500 etc.).
    void set_body(std::span<char> span, bool more = false);
    void set_body(std::string_view s, bool more = false);
    void set_body_owned(std::string body);
    void set_body_view(std::string_view s, bool more = false);
    // Convenience layer: implicitly set result(ok) + content-type. For custom status codes use set_body* above.
    void text(std::string_view body, std::string_view ctype = "text/plain");
    void json(std::string_view body);
    void text_view(std::string_view body, std::string_view ctype = "text/plain");
    void json_view(std::string_view body);

private:
    bool is_streamed() const { return is_streamed_; }
    void streamed(bool s) { is_streamed_ = s; }
};

}  // namespace lit
