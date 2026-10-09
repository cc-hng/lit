#include "lit/middleware/cors.h"

namespace lit {
namespace middleware {

net::awaitable<void>  //
cors(const request_t& req, response_t& res, const next_handler& go) {
    if (req.method() == http::verb::options) {
        res.result(http::status::no_content);
        res.set("Access-Control-Max-Age", "86400");
        res.set("Access-Control-Allow-Origin", "*");
        res.set("Access-Control-Allow-Headers", "*");
        res.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        co_return;
    }

    res.set("Access-Control-Allow-Origin", "*");
    co_await go();
}

}  // namespace middleware
}  // namespace lit
