#include "lit/middleware/serve_static.h"
#include "lit/middleware/detail/mime_types.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <inttypes.h>
#include <lit/object.h>
#include <string.h>
namespace fs = std::filesystem;

namespace {
std::string read_range(std::ifstream& f, size_t start, size_t end) {
    std::string out;
    if (!f || end < start) {
        return out;
    }

    const size_t len = end - start + 1;
    out.resize(len);

    f.seekg(start);
    f.read(out.data(), len);
    out.resize(f.gcount());  // trim in case EOF is reached earlier
    return out;
}

bool parse_range(std::string_view range, uint64_t size, uint64_t& start, uint64_t& end) {
    if (range.substr(0, 6) != "bytes=") return false;
    range.remove_prefix(6);
    const auto dash = range.find('-');
    if (dash == std::string_view::npos) return false;
    const auto a = range.substr(0, dash);
    const auto b = range.substr(dash + 1);
    const auto is_digits = [](std::string_view s) {
        return s.empty() || std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!is_digits(a) || !is_digits(b)) return false;
    try {
        const bool has_a = !a.empty(), has_b = !b.empty();
        if (!has_a && !has_b) return false;
        if (has_a) start = std::stoull(std::string(a));
        if (has_b) end   = std::stoull(std::string(b));
        if (!has_a) {          // bytes=-N : last N bytes
            start = size - end;
            end   = size - 1;
        }
        if (!has_b) {          // bytes=N- : from N, capped to a chunk
            end = start + lit::middleware::StaticFileProvider::CHUNK_SIZE - 1;
        }
        if (end >= size) end = size - 1;
        if (start > end || start >= size) return false;
    } catch (...) {
        return false;
    }
    return true;
}

inline bool str_is_directory(std::string_view path) {
#ifdef BOOST_MSVC
    return path.ends_with("\\");
#else
    return path.ends_with("/");
#endif
}

std::string pathcat(std::string_view base, std::string_view path) {
    if (base.empty()) return std::string(path);
    std::string res(base);
    char sep = '/';
#ifdef BOOST_MSVC
    sep = '\\';
#endif
    if (res.back() == sep) res.pop_back();
    res.append(path.data(), path.size());
#ifdef BOOST_MSVC
    std::replace(res.begin(), res.end(), '/', '\\');
#endif
    return res;
}

std::string render_directory(std::string_view mount_point, std::string_view doc_root,
                             std::string_view path) {
    std::string path0 = pathcat(doc_root, path);
    std::ostringstream oss;
    oss << R"(<!DOCTYPE html> <html> <head> </head> <body>)";
    oss << "<h1>Directory: " << doc_root << path << "</h1> <ul>";
    try {
        std::vector<std::string> entries;
        for (const auto& entry : fs::directory_iterator(path0)) {
            std::string name = entry.path().filename().string();
            if (fs::is_directory(entry)) {
                name += "/";
            }
            entries.emplace_back(std::move(name));
        }

        std::sort(entries.begin(), entries.end(),
                  [](const std::string& a, const std::string& b) {
                      if (a.back() == '/' || b.back() == '/') {
                          return a.back() == '/';
                      }

                      return a < b;
                  });

        for (const auto& entry : entries) {
            oss << "            <li><a href=\"" << mount_point << path << entry << "\">"
                << entry << "</a></li>\n";
        }
    } catch (const std::exception& e) {
        oss << "            <li>Error: " << e.what() << "</li>\n";
    }

    // HTML footer
    oss << R"(        </ul> </body> </html>)";
    return oss.str();
}

}  // namespace

namespace lit {
namespace middleware {

namespace detail {
// Return a reasonable mime type based on the extension of a file.
inline std::string_view get_mime_type(std::string_view path) {
    static auto get_ext = [](std::string_view path) {
        auto slash_pos = path.find_last_of("/\\");
        auto filename =
            (slash_pos == std::string_view::npos) ? path : path.substr(slash_pos + 1);

        auto dot_pos = filename.rfind('.');
        if (dot_pos == std::string_view::npos || dot_pos == 0) {
            return std::string_view{};
        }
        return filename.substr(dot_pos);
    };
    return lit::get_mime_type(get_ext(path));
}
}  // namespace detail

StaticFileProvider::StaticFileProvider(std::string_view mount_point, std::string_view dir)
  : mount_point_(mount_point)
  , doc_root_(dir) {
    if (!mount_point.empty() && mount_point.back() == '/') {
        mount_point_.pop_back();
    }
}

net::awaitable<void>  //
StaticFileProvider::operator()(const request_t& req, response_t& res,
                               const next_handler& go) {
    auto method = req.method();
    auto url    = req.get_url();
    if (method != http::verb::get && method != http::verb::head) [[unlikely]] {
        co_return co_await go();
    }

    if (!url.starts_with(mount_point_)) {
        co_return co_await go();
    }

    auto path = pathcat(doc_root_, url.substr(mount_point_.size()));
    // ponytail: canonicalize to resolve .. and symlinks; reject paths escaping doc_root.
    // Upgrade path: use fs::canonical (requires absolute input) if symlink resolution is desired.
    const auto canon = fs::weakly_canonical(path);
    const auto root   = fs::weakly_canonical(doc_root_);
    if (!canon.string().starts_with(root.string())) {
        co_return co_await go();
    }
    if (!fs::exists(canon)) {
        co_return co_await go();
    }

    if (str_is_directory(path)) {
        auto path0 = path;
        path.append("index.html");

        if (!fs::exists(path) && fs::is_directory(path0)) {
            auto content =
                render_directory(mount_point_, doc_root_, url.substr(mount_point_.size()));
            res.text(content, "text/html");
            co_return;
        }
    }

    res.set(http::field::content_type, detail::get_mime_type(path));
    // Attempt to open the file
    std::ifstream t(path.c_str());

    t.seekg(0, std::ios::end);
    size_t size = t.tellg();
    t.seekg(0, std::ios::beg);
    if (method == http::verb::head) {
        res.text_view("", detail::get_mime_type(path));
        res.content_length(size);
    } else {
        uint64_t start = 0, end = size - 1;

        // Send whole file
        if (req.find(http::field::range) == req.end()) {
            res.result(http::status::ok);
            res.set_body_owned({std::istreambuf_iterator<char>(t),
                                std::istreambuf_iterator<char>()});
            co_return;
        }

        if (parse_range(req[http::field::range], size, start, end)) {
            char buf[64] = {0};
            snprintf(buf, 64, "bytes %" PRIu64 "-%" PRIu64 "/%zu", start, end, size);
            res.result(http::status::partial_content);
            res.set(http::field::content_range, std::string(buf));
            res.set_body_owned(read_range(t, start, end));
        } else {
            res.result(http::status::range_not_satisfiable);
            res.set(http::field::content_range, "bytes */" + std::to_string(size));
        }
    }
}

}  // namespace middleware
}  // namespace lit
