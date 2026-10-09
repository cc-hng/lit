#pragma once

#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#include <boost/noncopyable.hpp>
#include <lit/object.h>

namespace lit {

namespace detail {

template <typename T, typename U = std::remove_cvref_t<T>>
static constexpr bool is_route_handler_v =
    std::is_invocable_r_v<net::awaitable<void>, U, const request_t&, response_t&,
                          const next_handler&>;

template <typename T>
struct is_awaitable : public std::false_type {};

template <typename T, typename Executor>
struct is_awaitable<boost::asio::awaitable<T, Executor>> : public std::true_type {};

template <typename T>
constexpr bool is_awaitable_v = is_awaitable<T>::value;

}  // namespace detail

class RouterContext;

class Router final : boost::noncopyable {
    using route_matcher =
        std::function<std::tuple<bool, std::optional<route_parameters>>(std::string_view)>;

    struct route_pattern {
        bool enabled;
        std::string literal;
        route_matcher matcher;
    };

    struct route_entry {
        http::verb method;
        route_pattern pattern;
        route_handler handler;
        bool needs_next;
    };

    std::vector<route_entry> routes_;

public:
    Router();
    ~Router();

    RouterContext group(std::string_view prefix);
    Router& use(route_handler&& h);

    template <typename H>
    std::enable_if_t<detail::is_route_handler_v<H>, Router&>
    route(http::verb method, std::string_view path, H&& h) {
        return add_route(method, compile_route(path), std::forward<H>(h), true);
    }

    template <typename H>
    std::enable_if_t<!detail::is_route_handler_v<H>, Router&>
    route(http::verb method, std::string_view path, H&& h) {
        return add_route(method, compile_route(path), make_handle(std::forward<H>(h)),
                         false);
    }

    template <typename H>
    inline Router& Get(std::string_view path, H&& h) {
        return route(http::verb::get, path, std::forward<H>(h));
    }

    template <typename H>
    inline Router& Post(std::string_view path, H&& h) {
        return route(http::verb::post, path, std::forward<H>(h));
    }

    template <typename H>
    inline Router& Any(std::string_view path, H&& h) {
        return route((http::verb)-1, path, std::forward<H>(h));
    }

    net::awaitable<void> handle(const request_t& req, response_t& res);
    net::awaitable<void> handle_from(size_t index, const request_t& req, response_t& resp);

private:
    Router& add_route(http::verb method, route_pattern pattern, route_handler&& h,
                      bool needs_next);

    template <typename Handler>
    static auto make_handle(Handler&& h) -> route_handler {
        static_assert(std::is_invocable_v<Handler, const request_t&, response_t&>,
                      "Unsupport handler!!!");
        using R = std::invoke_result_t<Handler, const request_t&, response_t&>;
        return {[h = std::forward<Handler>(h)]  //
                (const request_t& req, response_t& res,
                 const next_handler&) -> net::awaitable<void> {
                    if constexpr (detail::is_awaitable_v<R>) {
                        co_await h(req, res);
                    } else {
                        co_return h(req, res);
                    }
                }};
    }

    static route_pattern compile_route(std::string_view path);
};

class RouterContext : boost::noncopyable {
    std::string prefix_;
    Router& router_;

public:
    RouterContext(std::string_view prefix, Router& router);

    template <typename H>
    RouterContext& route(http::verb method, std::string_view path, H&& h) {
        router_.route(method, prefix_ + std::string(path), std::forward<H>(h));
        return *this;
    }

    template <typename H>
    RouterContext& Get(std::string_view path, H&& h) {
        return route(http::verb::get, path, std::forward<H>(h));
    }

    template <typename H>
    RouterContext& Post(std::string_view path, H&& h) {
        return route(http::verb::post, path, std::forward<H>(h));
    }

    template <typename H>
    RouterContext& Any(std::string_view path, H&& h) {
        return route((http::verb)-1, path, std::forward<H>(h));
    }
};

}  // namespace lit
