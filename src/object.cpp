#include "lit/object.h"
#include <algorithm>
#include <sstream>
#include <lit/util.h>

namespace lit {

bool request_t::compile_target() const {
    static std::string _nullstr = "";
    std::string_view _target    = target();
    auto offset                 = _target.find_first_of('?');
    std::string_view _query;
    if (offset != std::string_view::npos) {
        path   = _target.substr(0, offset);
        _query = _target.substr(offset + 1);
    } else {
        path   = _target;
        _query = _nullstr;
    }

    if (!_query.empty()) {
        auto pairs = str_split(_query, "&");
        std::unordered_map<std::string, std::string> out;
        for (const auto& kv : pairs) {
            auto pair = str_split(kv, "=");
            if (pair.size() != 2) {
                return false;
            }
            out.emplace(std::string(pair[0]), std::string(urldecode(pair[1])));
        }
        queries.emplace(std::move(out));
    }
    return true;
}

response_t::response_t(HttpStream* s)
  : is_streamed_(false)
  , buf_(buf_sm_)
  , capacity_(SMALL_CHUNK)
  , stream(s) {
}

response_t::~response_t() {
    if (buf_ && buf_ != buf_sm_) {
        delete[] buf_;
        buf_ = nullptr;
    }
}

void response_t::set_body(gsl::span<char> span, bool more) {
    set_body(std::string_view(span.data(), span.size()), more);
}

void response_t::set_body(std::string_view s, bool more) {
    auto& b = body();
    auto sz = s.size();
    ensure_buffer(sz);
    std::copy(s.begin(), s.end(), buf_);
    b.data = buf_;
    b.size = sz;
    b.more = more;

    if (!chunked()) {
        content_length(sz);
    }
}

void response_t::text(std::string_view body, std::string_view ctype) {
    result(http::status::ok);
    chunked(false);
    set_body(body);
    set(http::field::content_type, ctype);
}

void response_t::json(std::string_view body) {
    result(http::status::ok);
    chunked(false);
    set_body(body);
    set(http::field::content_type, "application/json");
}

void response_t::set_body_view(std::string_view s, bool more) {
    auto& b = body();
    b.data  = const_cast<char*>(s.data());
    b.more  = more;
    b.size  = s.size();

    if (!chunked()) {
        content_length(s.size());
    }
}

void response_t::text_view(std::string_view body, std::string_view ctype) {
    result(http::status::ok);
    chunked(false);
    set_body_view(body);
    set(http::field::content_type, ctype);
}

void response_t::json_view(std::string_view body) {
    result(http::status::ok);
    chunked(false);
    set_body_view(body);
    set(http::field::content_type, "application/json");
}

void response_t::ensure_buffer(int n) {
    if (capacity_ < n) {
        if (buf_ != buf_sm_) {
            delete[] buf_;
        }
        buf_      = new char[n];
        capacity_ = n;
    }
}

}  // namespace lit
