#pragma once

#include <string>
#include <string_view>

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/file_body.hpp>

namespace lit {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;

struct fetch_option_t {
    int  timeout   = 30;  // seconds
    bool keepalive = false;
    int  max_retry = 5;   // rebuild attempts when a keep-alive connection fails
    bool tls       = false;        // wrap the connection in TLS (https)
    bool verify    = true;         // verify peer cert + hostname
    std::string ca = "";           // optional CA file; empty = system default store
};

// Zero-copy outbound: host is a name or IP; body/headers are assembled in-place in req.
template <typename ReqBody, typename RespBody = http::string_body>
net::awaitable<http::response<RespBody>>
fetch(std::string_view host, int port, http::request<ReqBody> req,
      const fetch_option_t& opt = {});

// REST/JSON: text request/response body
extern template net::awaitable<http::response<http::string_body>>
fetch<http::string_body, http::string_body>(std::string_view, int,
        http::request<http::string_body>, const fetch_option_t&);

// Static download: empty request body, file response body
extern template net::awaitable<http::response<http::file_body>>
fetch<http::empty_body, http::file_body>(std::string_view, int,
        http::request<http::empty_body>, const fetch_option_t&);

void http_fetch_pool_cleanup();

}  // namespace lit
