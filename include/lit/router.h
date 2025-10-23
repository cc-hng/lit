#pragma once

#include <optional>
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
    using kv_t = std::optional<std::unordered_map<std::string, std::string>>;
    std::vector<route_handler> routes_;

public:
    Router();
    ~Router();

    RouterContext group(std::string_view prefix);

    Router& use(const route_handler& h);
    Router& use(route_handler&& h);

    template <typename H>
    std::enable_if_t<detail::is_route_handler_v<H>, Router&>
    route(http::verb method, std::string_view path, H&& h) {
        return use([method, parse_path = compile_route(path), h0 = std::forward<H>(h)](
                       const request_t& req, response_t& res,
                       const next_handler& go) mutable -> net::awaitable<void> {
            if (method != (http::verb)-1) {
                if (req.method() != method) {
                    co_return co_await go();
                }
            }

            auto [matched, params] = parse_path(req.path);
            if (!matched) {
                co_return co_await go();
            }

            req.params = std::move(params);
            co_await h0(req, res, go);
        });
    }

    template <typename H>
    std::enable_if_t<!detail::is_route_handler_v<H>, Router&>
    route(http::verb method, std::string_view path, H&& h) {
        return route(method, path, make_handle(std::forward<H>(h)));
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

    net::awaitable<void>
    operator()(const request_t& req, response_t& res, const next_handler& go = nullptr);

private:
    net::awaitable<void>
    handle_one(int index, const request_t& req, response_t& resp, const next_handler& go);

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

    /// @brief parse path to a kv map according to rule_t
    ///        throw a exception if parse failed.
    /// @param target:
    ///             "/user/:id/:name"
    ///             "/user/:id/:name?address=aa&phone=123456"
    ///         rule:
    /// @return /user/1/J -> {"id": "1", "name": "J"}
    static std::function<std::tuple<bool, kv_t>(std::string_view)>
    compile_route(std::string_view path, bool whole = true);
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
