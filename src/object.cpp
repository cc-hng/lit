#include "../include/lit/object.h"

#include <algorithm>

namespace lit {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool request_t::compile_target() const {
    if (target_compiled_) return target_valid_;

    const std::string_view raw_target = target();
    query_separator_                  = raw_target.find('?');
    queries_.clear();
    target_compiled_ = true;

    if (query_separator_ == std::string_view::npos) return true;

    const std::string_view query = raw_target.substr(query_separator_ + 1);
    if (query.empty()) return true;

    size_t begin = 0;
    while (begin <= query.size()) {
        const size_t end            = query.find('&', begin);
        const std::string_view item = query.substr(begin, end - begin);
        const size_t separator      = item.find('=');
        if (item.empty() || separator == std::string_view::npos) {
            target_valid_ = false;
            return false;
        }

        queries_.emplace(item.substr(0, separator), item.substr(separator + 1));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }

    return true;
}

std::string_view request_t::get_url() const {
    const std::string_view raw_target = target();
    return raw_target.substr(0, query_separator_);
}

std::string request_t::url_decode(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());

    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '+') {
            result.push_back(' ');
            continue;
        }

        if (sv[i] == '%' && i + 2 < sv.size()) {
            const int high = hex_value(sv[i + 1]);
            const int low  = hex_value(sv[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }

        result.push_back(sv[i]);
    }

    return result;
}

std::string request_t::get_query(std::string_view key) const {
    auto it = queries_.find(key);
    if (it == queries_.end()) return {};
    return url_decode(it->second);
}

std::string_view request_t::get_parameter(std::string_view key) const {
    for (const auto& [name, value] : parameters_) {
        if (name == key) return value;
    }
    return {};
}

std::string_view request_t::get_parameter(size_t index) const {
    if (index >= parameters_.size()) return {};
    return parameters_[index].second;
}

response_t::response_t(void* s, alloc_t const& alloc)
  : base_type(std::piecewise_construct, std::make_tuple(), std::make_tuple(alloc))
  , is_streamed_(false)
  , stream(s) {
    chunked(false);
}

response_t::~response_t() = default;

void response_t::set_body_owned(std::string new_body) {
    owned_body_    = std::make_shared<std::string>(std::move(new_body));
    auto& body_ref = this->body();
    body_ref.data  = const_cast<char*>(owned_body_->data());
    body_ref.size  = owned_body_->size();
    body_ref.more  = false;
    if (!chunked()) content_length(owned_body_->size());
}

void response_t::set_body(std::span<char> span, bool more) {
    set_body(std::string_view(span.data(), span.size()), more);
}

void response_t::set_body(std::string_view s, bool more) {
    buffer_.assign(s.begin(), s.end());

    auto& body_ref = body();
    body_ref.data  = buffer_.data();
    body_ref.size  = s.size();
    body_ref.more  = more;

    if (!chunked()) content_length(s.size());
}

void response_t::text(std::string_view body_value, std::string_view ctype) {
    result(http::status::ok);
    set_body(body_value);
    set(http::field::content_type, ctype);
}

void response_t::json(std::string_view body_value) {
    result(http::status::ok);
    set_body(body_value);
    set(http::field::content_type, "application/json");
}

void response_t::set_body_view(std::string_view s, bool more) {
    auto& body_ref = body();
    body_ref.data  = const_cast<char*>(s.data());
    body_ref.size  = s.size();
    body_ref.more  = more;

    if (!chunked()) content_length(s.size());
}
// WARNING: the caller owns the lifetime of `s`. If `s` refers to a temporary or local
// that is destroyed before the response is written, the socket write dereferences freed memory.
// Use set_body_owned() or set_body() when the source may not outlive this call.

void response_t::text_view(std::string_view body_value, std::string_view ctype) {
    result(http::status::ok);
    set_body_view(body_value);
    set(http::field::content_type, ctype);
}

void response_t::json_view(std::string_view body_value) {
    result(http::status::ok);
    set_body_view(body_value);
    set(http::field::content_type, "application/json");
}

}  // namespace lit
