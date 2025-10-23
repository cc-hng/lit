#include "lit/util.h"
#include <iomanip>
#include <sstream>

namespace lit {

std::string urlencode(std::string_view raw) {
    std::ostringstream oss;
    oss.fill('0');
    oss << std::hex;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            oss << '%' << std::setw(2) << std::uppercase << (int)c;
        }
    }
    return oss.str();
}

std::string urldecode(std::string_view raw) {
    std::string result;
    result.reserve(raw.size());

    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            int value;
            std::istringstream is(std::string(raw.substr(i + 1, 2)));
            if (is >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            }
        } else if (raw[i] == '+') {
            result += ' ';
        } else {
            result += raw[i];
        }
    }

    return result;
}

std::vector<std::string_view> str_split(std::string_view str, std::string_view delimiter) {
    size_t start = 0;
    size_t end   = 0;
    size_t dlen  = delimiter.size();
    std::vector<std::string_view> result;
    result.reserve(8);

    while ((end = str.find(delimiter, start)) != std::string_view::npos) {
        auto s = str.substr(start, end - start);
        result.emplace_back(s);
        start = end + dlen;
    }
    auto s = str.substr(start);
    result.emplace_back(s);
    return result;
}

}  // namespace lit
