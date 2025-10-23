#pragma once

#include <string>
#include <string_view>
#include <lit/object.h>

namespace lit {
namespace middleware {

class StaticFileProvider {
    std::string mount_point_;
    std::string doc_root_;

public:
    enum { CHUNK_SIZE = 128 * 1024 };

    StaticFileProvider(std::string_view mount_point, std::string_view dir);

    net::awaitable<void>  //
    operator()(const request_t& req, response_t& res, const next_handler& go);
};

}  // namespace middleware
}  // namespace lit
