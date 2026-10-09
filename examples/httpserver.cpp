
#if LIT_USE_SSL
#    include "common.h"
#endif
#include <fmt/format.h>
#include <lit.hpp>
#include <lit/multipart_parser.h>

#ifdef HTTP_STREAM
// #include <boost/beast/http.hpp>
#    include <boost/beast/http/write.hpp>
#endif
//
namespace net  = boost::asio;
namespace http = boost::beast::http;

net::awaitable<void>  //
timeout(int ms) {
    net::steady_timer timer(co_await net::this_coro::executor);
    timer.expires_after(std::chrono::milliseconds(ms));
    co_await timer.async_wait(net::use_awaitable);
}

template <bool SSL>
void start(lit::TemplatedApp<SSL>& app) {
    using stream_type = typename lit::TemplatedApp<SSL>::stream_type;

    app.on_error([](std::string_view msg) { fmt::print("error. {}\n", msg); });
    // app.use(lit::middleware::cors);

    app.Get("/api/a", [](const lit::request_t& req, lit::response_t& res) {
        res.text_view("Hello, a!");
    });

#ifdef HTTP_STREAM
    app.stream(
        "/api/stream",
        [](const lit::request_t& req, lit::response_t& res,
           const lit::next_handler& go) -> net::awaitable<void> {
            res.result(http::status::ok);
            res.chunked(true);

            auto* stream                  = (stream_type*)(res.stream);
            std::vector<std::string> msgs = {
                R"({"a":1, "b":2})", R"({"a":2, "b":2})", R"({"a":3, "b":2})",
                R"({"a":4, "b":2})", R"({"a":5, "b":2})", R"({"a":6, "b":2})",
            };

            // Beast buffer_body streaming contract: prime the writer with
            // data=nullptr + more=true BEFORE constructing the serializer /
            // writing the header; only then feed per-chunk buffers.
            res.body().data = nullptr;
            res.body().more = true;

            http::response_serializer<http::buffer_body, http::basic_fields<lit::alloc_t>> sr{
                res};

            co_await http::async_write_header(*stream, sr, net::use_awaitable);

            for (const auto& m : msgs) {
                res.body().data = const_cast<char*>(m.data());
                res.body().size = m.size();
                res.body().more = true;
                try {
                    co_await http::async_write(*stream, sr, net::use_awaitable);
                } catch (boost::system::system_error& e) {
                    if (e.code() != http::error::need_buffer) {
                        throw;
                    }
                }
                co_await timeout(200);
            }

            res.body().data = nullptr;
            res.body().more = false;
            co_await http::async_write(*stream, sr, net::use_awaitable);
        });
#endif

    auto g = app.group("/api");
    g.Get("/b",
          [](const lit::request_t& req, lit::response_t& res) { res.text("Hello, b!"); });

    // Query: print all query params (per-key API)
    g.Get("/param", [](const lit::request_t& req, lit::response_t& res) {
        fmt::print("query a={}, b={}\n", req.get_query("a"), req.get_query("b"));
        res.text(fmt::format("params: a={}, b={}", req.get_query("a"), req.get_query("b")));
    });

    // Path parameters
    g.Get("/:svc/:method", [](const lit::request_t& req, lit::response_t& res) {
        auto svc    = req.get_parameter("svc");
        auto method = req.get_parameter("method");

        fmt::print("route: svc={}, method={}\n", svc, method);
        res.text(fmt::format("route: svc={}, method={}", svc, method));
    });

    // multipart/form-data 上传示例：零拷贝流式解析，字段与文件分开处理
    // curl -F "user=alice" -F "avatar=@/path/to/a.png;type=image/png" http://127.0.0.1:8088/api/upload
    g.Post("/upload", [](const lit::request_t& req, lit::response_t& resp) {
        bool ok =
            lit::multipart_decode(req.at(http::field::content_type), req.body(),
                                  [&](std::string_view name, std::string_view filename,
                                      std::string_view content_type,
                                      std::string_view content) {
                                      fmt::print("file:  {} | {} | {} bytes\n", filename,
                                                 content_type, content.size());
                                  });
        if (!ok) {
            resp.result(http::status::bad_request);
            resp.set_body_view(R"({"code":400,"error":"not multipart/form-data"})");
            return;
        }
        resp.json_view(R"({"code":200,"data":{}})");
    });

    app.serve_static("/static", "/data/www/html");
    app.start("0.0.0.0", 8088);
}

int main(int argc, char** argv) {
    net::io_context ioc;

#if LIT_USE_SSL
    if (argc < 2) {
        fmt::print("Usage: {} <tls>\n", argv[0]);
        return 1;
    }
    bool tls = std::stoi(argv[1]);
    if (tls) {
        lit::SSLApp app(ioc, lit::SSLApp::option_t{
                                 .ssl_method = (int)net::ssl::context::tlsv12,
                                 .ssl_cert   = ssl::cert,
                                 .ssl_key    = ssl::key,
                                 .ssl_dh     = ssl::dh,
                             });
        start(app);
        ioc.run();
    } else {
        lit::App app(ioc);
        start(app);
        ioc.run();
    }
#else
    lit::App app(ioc);
    start(app);
    ioc.run();
#endif

    return 0;
}
