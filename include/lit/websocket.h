#pragma once

#include <boost/beast/websocket.hpp>
#include <lit/server.h>

namespace lit {

template <bool SSL>
template <typename H>
TemplatedApp<SSL>& TemplatedApp<SSL>::websocket(std::string_view path, H&& h) {
    router_.route((http::verb)-1, path,
                  [h = std::forward<H>(h)](const request_t& req,  //
                                           response_t& res, const next_handler& go)
                      -> net::awaitable<void> {
                      stream_expires_after(static_cast<stream_type*>(res.stream), -1);
                      res.keep_alive(true);
                      res.streamed(true);
                      auto* p = static_cast<stream_type*>(res.stream);
                      beast::websocket::stream<stream_type> ws{std::move(*p)};
                      // Stream moved into ws; nulled so do_session won't touch it on keep-alive loop continuation.
                      res.stream = nullptr;
                      beast::get_lowest_layer(ws).expires_never();
                      ws.set_option(beast::websocket::stream_base::timeout::suggested(
                          beast::role_type::server));
                      co_await ws.async_accept(req);
                      co_await std::move(h)(req, std::move(ws));
                  });
    return *this;
}

}  // namespace lit
