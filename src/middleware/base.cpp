#include "lit/middleware/base.h"

namespace lit {
namespace middleware {

net::awaitable<void>
auto_headers(const request_t& req, response_t& res, const next_handler& go) {
    res.result(http::status::not_found);
    res.version(req.version());
    res.keep_alive(req.keep_alive());
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set_body_view("");

    co_await go();

    if (res.result() == http::status::not_found) {
        res.set_body_view("Not found\n");
    }

    if (res.find(http::field::content_type) == res.end()) {
        res.set(http::field::content_type, "text/plain");
    }
}

net::awaitable<void>  //
cors(const request_t& req, response_t& res, const next_handler& go) {
    if (req.method() == http::verb::options) {
        res.text_view("");
        res.set("Access-Control-Max-Age", "86400");
    }

    res.set("Access-Control-Allow-Origin", "*");
    res.set("Access-Control-Allow-Headers", "*");
    res.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    co_await go();
}

}  // namespace middleware
}  // namespace lit
