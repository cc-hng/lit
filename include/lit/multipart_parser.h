#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace lit {

/* Streaming, zero-copy multipart parser modeled on uWebSockets' Multipart.h.
 * All returned views point into the body passed to setBody(). Single pass. */
class MultipartParser {
public:
    /* From a "multipart/form-data; boundary=..." content-type. Check isValid(). */
    explicit MultipartParser(std::string_view content_type);

    bool isValid() const;

    /* body 中出现过 boundary（含关闭 boundary）即为有效请求；decode 用它区分畸形 body */
    bool sawBoundary() const;

    void setBody(std::string_view body);

    struct Part {
        std::string_view name;
        std::string_view filename;
        std::string_view content_type;
        std::string_view body;
    };

    /* Next part's headers + body, or nullopt on end/error. */
    std::optional<Part> getNextPart();

private:
    static void parse_headers(std::string_view headers, Part& out);

    std::string boundary_;  // "--" + boundary
    std::string_view rest_;
    bool first_   = true;
    bool saw_bdy_ = false;
};

/* 一键解析：每部分回调 handle(name, filename, content_type, content)。
 * 参数均为指向 body 的 string_view，零拷贝。返回 false = 非 multipart/form-data 或解析失败。 */
template <typename Fn>
inline bool multipart_decode(std::string_view content_type, std::string_view body, Fn&& handle) {
    MultipartParser parser(content_type);
    if (!parser.isValid()) {
        return false;
    }
    parser.setBody(body);
    while (auto part = parser.getNextPart()) {
        handle(part->name, part->filename, part->content_type, part->body);
    }
    return parser.sawBoundary();  // body 里从未出现 boundary = 畸形请求
}

/* One part's data for multipart_encode(). Views, not copies. */
struct multipart_part_t {
    std::string_view name;
    std::string_view filename;      /* empty = plain form field */
    std::string_view content_type;  /* empty = header omitted */
    std::string_view content;
};

/* Build a multipart/form-data body string. 实现见 src/multipart_parser.cpp */
std::string multipart_encode(std::string_view boundary,
                             std::initializer_list<multipart_part_t> parts);

}  // namespace lit
