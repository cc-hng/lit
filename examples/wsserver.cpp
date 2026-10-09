#if LIT_USE_SSL
#    include "common.h"
#endif
#include <boost/asio.hpp>
#include <fmt/format.h>
#include <lit.hpp>
#include <lit/websocket.h>

namespace net = boost::asio;

template <bool SSL>
void start(lit::TemplatedApp<SSL>& app) {
    using stream_type = typename lit::TemplatedApp<SSL>::stream_type;
    namespace beast   = boost::beast;
    app.on_error([](std::string_view msg) { fmt::print("error. {}\n", msg); });

    app.websocket("/ws/echo",
                  [](const lit::request_t& req,
                     beast::websocket::stream<stream_type>&& ws) -> net::awaitable<void> {
                      for (;;) {
                          // This buffer will hold the incoming message
                          beast::flat_buffer buffer;

                          // Read a message
                          co_await ws.async_read(buffer);

                          // Echo the message back
                          ws.text(ws.got_text());
                          co_await ws.async_write(buffer.data());
                      }
                  });

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
