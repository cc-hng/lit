
#pragma once

#include <lit/object.h>

namespace lit {
namespace middleware {

net::awaitable<void>  //
cors(const request_t& req, response_t& res, const next_handler& go);

}  // namespace middleware
}  // namespace lit
