#include <cctype>
#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <lit.hpp>
#include <lit/client.h>
#include <lit/multipart_parser.h>
#include <gsl/gsl>

namespace net = boost::asio;

// Usage: httpclient [--tls] [--ca=FILE] [HOST] [PORT]
int main(int argc, char** argv) {
    bool        tls = false;
    bool        verify = true;
    std::string ca;
    std::string host = "127.0.0.1";
    int         port = 8088;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tls") {
            tls = true;
        } else if (a == "--insecure") {
            verify = false;
        } else if (a.rfind("--ca=", 0) == 0) {
            ca = a.substr(5);
        } else if (std::isdigit(a[0])) {
            port = std::stoi(a);
        } else {
            host = a;
        }
    }

    auto defer = gsl::finally([] { lit::http_fetch_pool_cleanup(); });
    net::io_context ioc;

    std::cout << (tls ? "[TLS] " : "[PLAIN] ") << host << ":" << port
              << (ca.empty() ? "" : " ca=" + ca) << std::endl;

    lit::fetch_option_t opt{.timeout = 10, .keepalive = true,
                            .tls = tls, .verify = verify, .ca = ca};

    net::co_spawn(ioc, [&]() -> net::awaitable<void> {
        try {
        // GET: keepalive
        lit::http::request<lit::http::string_body> req{lit::http::verb::get, "/", 11};
        auto resp = co_await lit::fetch(host, port, std::move(req), opt);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- ---- -------------------" << std::endl;
        req = lit::http::request<lit::http::string_body>{lit::http::verb::get, "/api/a", 11};
        resp = co_await lit::fetch(host, port, std::move(req), opt);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- ---- -------------------" << std::endl;
        req = lit::http::request<lit::http::string_body>{
            lit::http::verb::get, "/api/param?a=1&b=2", 11};
        resp = co_await lit::fetch(host, port, std::move(req), opt);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- ---- -------------------" << std::endl;
        req = lit::http::request<lit::http::string_body>{
            lit::http::verb::get, "/api/user/login", 11};
        resp = co_await lit::fetch(host, port, std::move(req), opt);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- ---- -------------------" << std::endl;
        // POST: body moved in, zero-copy
        req = lit::http::request<lit::http::string_body>{
            lit::http::verb::post, "/api/c", 11};
        req.body() = "{}";
        resp = co_await lit::fetch(host, port, std::move(req), opt);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- ---- -------------------" << std::endl;
        // multipart upload: build body + content-type by hand
        const std::string boundary = "----WebKitFormBoundarylit";
        auto body = lit::multipart_encode(
            boundary, {{"field1", "", "text/plain", "This is the content of field1"},
                       {"file1", "example.txt", "text/plain",
                        "This is the content of example.txt"},
                       {"field2", "", "text/plain",
                        "This is the content of field2"}});
        req = lit::http::request<lit::http::string_body>{
            lit::http::verb::post, "/api/upload", 11};
        req.body() = std::move(body);
        req.set(lit::http::field::content_type,
                "multipart/form-data; boundary=" + boundary);
        resp = co_await lit::fetch(host, port, std::move(req), opt);
        std::cout << resp.body() << std::endl;
        } catch (std::exception& ex) {
            std::cout << "fetch error: " << ex.what() << std::endl;
        }
    }, net::detached);

    ioc.run();
    return 0;
}
