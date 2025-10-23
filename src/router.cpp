#include "lit/router.h"
#include <regex>

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

Router& Router::use(const route_handler& h) {
    routes_.emplace_back(h);
    return *this;
}

Router& Router::use(route_handler&& h) {
    routes_.emplace_back(std::move(h));
    return *this;
}

RouterContext Router::group(std::string_view prefix) {
    return RouterContext(prefix, *this);
}

net::awaitable<void>
Router::operator()(const request_t& req, response_t& res, const next_handler& go) {
    if (GSL_LIKELY(req.compile_target())) {
        try {
            co_await handle_one(0, req, res, go);
        } catch (std::exception& e) {
            res.result(http::status::internal_server_error);
            res.text(e.what());
        }
    } else {
        res.result(http::status::bad_request);
        res.text("Illegal request-target\n");
    }
}

net::awaitable<void>  //
Router::handle_one(int index, const request_t& req, response_t& resp,
                   const next_handler& go) {
    if (GSL_UNLIKELY(index >= routes_.size())) {
        if (go) {
            co_await go();
        }
        co_return;
    }

    const auto& fn = routes_.at(index);
    auto next      = [&, this, index]() -> net::awaitable<void> {
        co_await handle_one(index + 1, req, resp, nullptr);
    };
    co_await fn(req, resp, next);
}

std::function<std::tuple<bool, Router::kv_t>(std::string_view)>
Router::compile_route(std::string_view path, bool whole) {
    class ParsePath {
    public:
        ParsePath() = default;

        void init(std::string_view path, bool whole) {
            bool is_regex                         = false;
            std::string regex                     = std::string(path);
            std::string_view::size_type start_pos = 0;
            while (start_pos != std::string_view::npos) {
                start_pos = regex.find(':', start_pos);
                if (start_pos != std::string_view::npos) {
                    const auto next_end_pos  = regex.find(':', start_pos + 1);
                    const auto slash_end_pos = regex.find('/', start_pos);
                    const auto end_pos       = std::min(slash_end_pos, next_end_pos);
                    const auto key_name =
                        regex.substr(start_pos + 1, end_pos - start_pos - 1);

                    if (GSL_UNLIKELY(key_name.empty())) {
                        throw std::runtime_error(std::string("Empty parameter name "
                                                             "found "
                                                             "in path: ")
                                                 + path.data());
                    }

                    if (next_end_pos != std::string_view::npos && end_pos == next_end_pos) {
                        regex.replace(start_pos, end_pos - start_pos + 1, "(.+)");
                    } else {
                        regex.replace(start_pos, end_pos - start_pos, "([^/\\s]+)");
                    }
                    keys_.emplace_back(key_name);
                    is_regex = true;
                }
            }
            regex_str_ = regex;
            if (is_regex) {
                regex = "^" + regex;
                if (whole) {
                    regex += "$";
                }
                re_ = std::make_shared<std::regex>(regex);
            }
        }

        std::tuple<bool, kv_t> operator()(std::string_view raw) {
            if (!re_) {
                return std::make_tuple(std::string_view(regex_str_) == raw, std::nullopt);
            }

            std::cmatch cm;
            auto p = static_cast<const char*>(raw.data());
            if (!std::regex_match(p, p + raw.size(), cm, *re_)) {
                return std::make_tuple(false, std::nullopt);
            }

            if (cm.size() - 1 != keys_.size()) {
                return std::make_tuple(false, std::nullopt);
            }

            typename kv_t::value_type params;
            int i = 1;
            for (auto key : keys_) {
                params.emplace(std::move(key), cm[i++].str());
            }
            return std::make_tuple(true, params);
        }

    private:
        std::string regex_str_;
        std::shared_ptr<std::regex> re_ = nullptr;
        std::vector<std::string> keys_;
    };

    ParsePath pp;
    pp.init(path, whole);
    return std::move(pp);
}

}  // namespace lit
