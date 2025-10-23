#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <lit/multipart_parser.h>

namespace lit {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;

struct fetch_option_t {
    using method_type  = boost::beast::http::verb;
    using headers_type = boost::beast::http::fields;

    method_type method   = method_type::get;
    headers_type headers = {};
    std::string body     = "";
    int timeout          = 30;  // seconds
    bool keepalive       = false;
    int max_retry        = 5;
};

net::awaitable<http::response<http::string_body>>
http_get(std::string_view url, bool keepalive = false,
         const fetch_option_t::headers_type& headers = {});

net::awaitable<http::response<http::string_body>>
http_post(std::string_view url, std::string body, bool keepalive = false,
          const fetch_option_t::headers_type& headers = {});

net::awaitable<http::response<http::string_body>>
http_upload(std::string_view url, const std::vector<multipart_formdata_t>& formdata,
            bool keepalive = false, const fetch_option_t::headers_type& headers = {});

void http_fetch_pool_cleanup();

}  // namespace lit
