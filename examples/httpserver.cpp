
#if LIT_WITH_OPENSSL
#    include "common.h"
#endif
#include <boost/asio.hpp>
#include <fmt/core.h>
#include <lit.hpp>
#include <lit/multipart_parser.h>

namespace net  = boost::asio;
namespace http = boost::beast::http;

net::awaitable<void>  //
timeout(int ms) {
    net::steady_timer timer(co_await net::this_coro::executor);
    timer.expires_after(std::chrono::milliseconds(ms));
    co_await timer.async_wait(net::use_awaitable);
}

int main(int argc, char** argv) {
#if LIT_WITH_OPENSSL
    if (argc < 2) {
        fmt::print("Usage: {} <tls>\n", argv[0]);
        return 1;
    }
    bool tls = std::stoi(argv[1]);
#endif

    net::io_context ioc;

    lit::App app(ioc);

    app.on_error([](std::string_view msg) { fmt::print("error. {}\n", msg); });
    app.use(lit::middleware::cors);

    app.Get("/api/a", [](const lit::request_t& req, lit::response_t& res) {
        res.text_view("Hello, a!");
    });

    app.stream("/api/stream",
               [](const lit::request_t& req, lit::response_t& res,
                  const lit::next_handler& go) -> net::awaitable<void> {
                   res.result(http::status::ok);
                   res.chunked(true);

                   http::response_serializer<http::buffer_body> sr{res};
                   co_await res.stream->async_write_header(sr);

                   std::vector<std::string> msgs = {
                       R"({"a":1, "b":2})", R"({"a":2, "b":2})", R"({"a":3, "b":2})",
                       R"({"a":4, "b":2})", R"({"a":5, "b":2})", R"({"a":6, "b":2})",
                   };

                   for (const auto& m : msgs) {
                       res.set_body_view(m, true);
                       try {
                           co_await res.stream->async_write_body(sr);
                       } catch (boost::system::system_error& e) {
                           if (e.code() != http::error::need_buffer) {
                               throw;
                           }
                       }
                       co_await timeout(120 * 1000);
                   }

                   res.set_body_view("");
                   res.body().data = nullptr;
                   co_await res.stream->async_write_body(sr);
               });

    auto g = app.group("/api");
    g.Get("/b",
          [](const lit::request_t& req, lit::response_t& res) { res.text("Hello, b!"); });

    g.Post("/upload", [](const lit::request_t& req, lit::response_t& resp) {
        auto result = lit::multipart_formdata_t::decode(req);
        for (const auto& f : result) {
            fmt::print("name:{}, filename:{}, content_type:{}\n", f.name, f.filename,
                       f.content_type);
            fmt::print("content:{}\n", f.content);
            fmt::print("---------------------------------------------------------\n");
        }
        resp.json_view(R"({"code":200,"data":{}})");
    });

    app.serve_static("/static", "/data/www/html");

#if LIT_WITH_OPENSSL
    if (tls) {
        app.start("0.0.0.0", 8088,
                  lit::App::tls_option_t{
                      .method = (int)net::ssl::context::tlsv12,
                      .cert   = ssl::cert,
                      .key    = ssl::key,
                      .dh     = ssl::dh,
                  });
    } else {
        app.start("0.0.0.0", 8088);
    }
#else
    app.start("0.0.0.0", 8088);
#endif

    ioc.run();
    return 0;
}
