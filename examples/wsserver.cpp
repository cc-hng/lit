#if LIT_WITH_OPENSSL
#    include "common.h"
#endif
#include <boost/asio.hpp>
#include <fmt/core.h>
#include <lit.hpp>

namespace net = boost::asio;

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

    app.websocket("/ws/echo", [](const auto& req, auto&& ws) -> net::awaitable<void> {
        for (;;) {
            // This buffer will hold the incoming message
            boost::beast::flat_buffer buffer;

            // Read a message
            co_await ws.async_read(buffer);

            // Echo the message back
            ws.text(ws.got_text());
            co_await ws.async_write(buffer.data());
        }
    });

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
