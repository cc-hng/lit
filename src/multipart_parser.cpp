/* multipart/form-data 解析与编码实现。声明见 include/lit/multipart_parser.h */
#include "lit/multipart_parser.h"

#include <cctype>
#include <string>

namespace lit {

namespace {

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline void ltrim(std::string_view& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
}

inline void rtrim(std::string_view& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
}

/* Extract boundary from "multipart/form-data; boundary=..." (quoted values ok). */
std::string_view multipart_parse_boundary(std::string_view ct) {
    auto semi = ct.find(';');
    std::string_view main_type = ct.substr(0, semi);
    rtrim(main_type);
    if (!iequals(main_type, "multipart/form-data")) {
        return {};
    }

    while (semi != std::string_view::npos) {
        std::string_view param = ct.substr(semi + 1);
        auto next = param.find(';');
        param = param.substr(0, next);

        ltrim(param);
        rtrim(param);

        auto eq = param.find('=');
        if (eq != std::string_view::npos) {
            std::string_view key = param.substr(0, eq);
            rtrim(key);
            if (iequals(key, "boundary")) {
                std::string_view val = param.substr(eq + 1);
                ltrim(val);
                rtrim(val);
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val.remove_prefix(1);
                    val.remove_suffix(1);
                }
                return val;
            }
        }
        semi = next == std::string_view::npos ? std::string_view::npos : semi + 1 + next;
    }
    return {};
}

/* "form-data; name=\"n\"; filename=\"f\"" -> name/filename views */
void parse_content_disposition(std::string_view line, std::string_view& name,
                               std::string_view& filename) {
    auto semi = line.find(';');
    if (semi == std::string_view::npos) {
        return;
    }
    line.remove_prefix(semi + 1);

    while (!line.empty()) {
        ltrim(line);
        if (!line.empty() && line.front() == ';') {  // quoted value 分支不消费 ';'
            line.remove_prefix(1);
            ltrim(line);
        }
        if (line.empty()) {
            break;
        }

        auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            break;
        }

        std::string_view key = line.substr(0, eq);
        rtrim(key);
        line.remove_prefix(eq + 1);

        std::string_view value;
        if (!line.empty() && line.front() == '"') {
            line.remove_prefix(1);
            auto quote = line.find('"');
            value = line.substr(0, quote);
            line.remove_prefix(quote == std::string_view::npos ? line.size() : quote + 1);
        } else {
            auto semi2 = line.find(';');
            value = line.substr(0, semi2);
            line.remove_prefix(semi2 == std::string_view::npos ? line.size() : semi2 + 1);
        }

        if (iequals(key, "name")) {
            name = value;
        } else if (iequals(key, "filename")) {
            filename = value;
        }
    }
}

}  // namespace

MultipartParser::MultipartParser(std::string_view content_type)
    : boundary_("--" + std::string(multipart_parse_boundary(content_type))) {}

bool MultipartParser::isValid() const { return boundary_.size() > 2; }

bool MultipartParser::sawBoundary() const { return saw_bdy_; }

void MultipartParser::setBody(std::string_view body) { rest_ = body; }

std::optional<MultipartParser::Part> MultipartParser::getNextPart() {
    if (rest_.size() < boundary_.size()) {
        return std::nullopt;
    }

    if (first_) {
        auto pos = rest_.find(boundary_);
        if (pos == std::string_view::npos) {
            return std::nullopt;  // no opening boundary
        }
        rest_.remove_prefix(pos + boundary_.size());
        first_    = false;
        saw_bdy_ = true;
    }

    auto end = rest_.find(boundary_);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    saw_bdy_ = true;

    std::string_view part = rest_.substr(0, end);
    rest_.remove_prefix(end + boundary_.size());

    /* part = "\r\n" + headers + "\r\n\r\n" + body + "\r\n" */
    if (part.size() < 4) {
        return std::nullopt;
    }
    part.remove_prefix(2);  // strip leading CRLF
    part.remove_suffix(2);  // strip CRLF before next boundary

    auto headers_end = part.find("\r\n\r\n");
    if (headers_end == std::string_view::npos) {
        return std::nullopt;
    }

    Part out;
    parse_headers(part.substr(0, headers_end), out);
    out.body = part.substr(headers_end + 4);
    return out;
}

void MultipartParser::parse_headers(std::string_view headers, Part& out) {
    size_t pos = 0;
    while (pos < headers.size()) {
        auto eol = headers.find("\r\n", pos);
        std::string_view line =
            headers.substr(pos, eol == std::string_view::npos ? std::string_view::npos
                                                              : eol - pos);
        pos = eol == std::string_view::npos ? headers.size() : eol + 2;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string_view key   = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        ltrim(value);
        rtrim(value);

        if (iequals(key, "content-disposition")) {
            parse_content_disposition(value, out.name, out.filename);
        } else if (iequals(key, "content-type")) {
            out.content_type = value;
        }
    }
}

std::string multipart_encode(std::string_view boundary,
                             std::initializer_list<multipart_part_t> parts) {
    std::string body;
    for (const auto& p : parts) {
        body += "--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"";
        body += p.name;
        body += "\"";
        if (!p.filename.empty()) {
            body += "; filename=\"";
            body += p.filename;
            body += "\"";
        }
        body += "\r\n";
        if (!p.content_type.empty()) {
            body += "Content-Type: ";
            body += p.content_type;
            body += "\r\n";
        }
        body += "\r\n";
        body += p.content;
        body += "\r\n";
    }
    body += "--";
    body += boundary;
    body += "--\r\n";
    return body;
}

}  // namespace lit
