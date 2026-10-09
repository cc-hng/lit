#include "lit/router.h"
#include <cctype>
#include <stdexcept>

namespace lit {

RouterContext::RouterContext(std::string_view prefix, Router& router)
  : prefix_(prefix)
  , router_(router) {
}

Router::Router() {
    routes_.reserve(64);
}

Router::~Router() {
}

Router& Router::use(route_handler&& h) {
    routes_.push_back({
        (http::verb)-1, {false, {}, {}},
         std::move(h), true
    });
    return *this;
}

Router& Router::add_route(http::verb method, route_pattern pattern, route_handler&& h,
                          bool needs_next) {
    routes_.push_back({method, std::move(pattern), std::move(h), needs_next});
    return *this;
}

RouterContext Router::group(std::string_view prefix) {
    return RouterContext(prefix, *this);
}

net::awaitable<void> Router::handle(const request_t& req, response_t& res) {
    if (req.compile_target()) [[likely]] {
        try {
            co_await handle_from(0, req, res);
        } catch (std::exception& e) {
            res.result(http::status::internal_server_error);
            res.text(e.what());
        }
    } else {
        res.result(http::status::bad_request);
        res.text("Illegal request-target\n");
    }
}

// Iterative dispatch: the hot path (no middleware chain) is a plain loop with
// zero coroutine frames per route miss. Only handlers that call their `next`
// (middleware) drop into the small recursive chain handler.
net::awaitable<void> Router::handle_from(size_t index, const request_t& req,
                                         response_t& resp) {
    const auto url = req.get_url();
    for (size_t i = index; i < routes_.size(); ++i) {
        auto& entry = routes_[i];
        if (entry.method != (http::verb)-1 && req.method() != entry.method) {
            continue;
        }

        if (entry.pattern.enabled) {
            req.parameters_.clear();
            if (entry.pattern.matcher) {
                auto [matched, params] = entry.pattern.matcher(url);
                if (!matched) {
                    continue;
                }
                if (params) {
                    req.parameters_ = std::move(*params);
                }
            } else if (entry.pattern.literal != url) {
                continue;
            }
        }

        if (!entry.needs_next) {
            const next_handler empty_next{};
            co_await entry.handler(req, resp, empty_next);
            co_return;
        }

        next_handler next = [&]() { return handle_from(i + 1, req, resp); };
        co_await entry.handler(req, resp, next);
        co_return;
    }
}
namespace {

struct route_segment {
    enum class type { literal, parameter, wildcard };

    type kind;
    std::string value;
};

bool contains_space(std::string_view value) {
    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            return true;
        }
    }
    return false;
}

bool valid_parameter_name(std::string_view name) {
    return !name.empty() && name.find_first_of(":*") == std::string_view::npos;
}

}  // namespace

Router::route_pattern Router::compile_route(std::string_view path) {
    class ParsePath {
    public:
        void init(std::string_view path) {
            path_        = std::string(path);
            size_t start = 0;
            while (true) {
                const auto slash = path_.find('/', start);
                const auto end   = slash == std::string::npos ? path_.size() : slash;
                const std::string_view segment(path_.data() + start, end - start);

                if (!segment.empty() && segment.front() == ':') {
                    const auto name = segment.substr(1);
                    if (!valid_parameter_name(name)) [[unlikely]] {
                        throw std::runtime_error("Invalid parameter name in path: " + path_);
                    }
                    segments_.push_back({route_segment::type::parameter, std::string(name)});
                    dynamic_ = true;
                } else if (!segment.empty() && segment.front() == '*') {
                    const auto name = segment.substr(1);
                    if (slash != std::string::npos || !valid_parameter_name(name))
                        [[unlikely]] {
                        throw std::runtime_error("Invalid wildcard parameter in path: "
                                                 + path_);
                    }
                    segments_.push_back({route_segment::type::wildcard, std::string(name)});
                    dynamic_ = true;
                } else {
                    segments_.push_back(
                        {route_segment::type::literal, std::string(segment)});
                }

                if (slash == std::string::npos) {
                    break;
                }
                start = slash + 1;
            }
        }

        std::tuple<bool, std::optional<route_parameters>>
        operator()(std::string_view raw) const {
            if (!dynamic_) {
                return {raw == path_, std::nullopt};
            }

            route_parameters params;
            size_t raw_start = 0;
            for (size_t i = 0; i < segments_.size(); ++i) {
                const auto& segment = segments_[i];
                const auto slash    = raw.find('/', raw_start);

                if (segment.kind == route_segment::type::wildcard) {
                    if (i + 1 != segments_.size() || raw_start == raw.size()) {
                        return {false, std::nullopt};
                    }
                    const auto value = raw.substr(raw_start);
                    params.emplace_back(segment.value, std::string(value));
                    return {true, params};
                }

                const auto raw_end = slash == std::string::npos ? raw.size() : slash;
                const auto value   = raw.substr(raw_start, raw_end - raw_start);
                if (segment.kind == route_segment::type::parameter) {
                    if (value.empty() || contains_space(value)) {
                        return {false, std::nullopt};
                    }
                    params.emplace_back(segment.value, std::string(value));
                } else if (value != segment.value) {
                    return {false, std::nullopt};
                }

                if (i + 1 != segments_.size()) {
                    if (slash == std::string::npos) {
                        return {false, std::nullopt};
                    }
                    raw_start = slash + 1;
                } else if (slash != std::string::npos) {
                    return {false, std::nullopt};
                }
            }

            return {true, params};
        }

        bool dynamic() const { return dynamic_; }
        const std::string& path() const { return path_; }

    private:
        std::string path_;
        std::vector<route_segment> segments_;
        bool dynamic_ = false;
    };

    ParsePath pp;
    pp.init(path);
    if (!pp.dynamic()) {
        return route_pattern{true, pp.path(), {}};
    }
    return route_pattern{true, {}, [pp = std::move(pp)](std::string_view raw) {
                             return pp(raw);
                         }};
}

}  // namespace lit
