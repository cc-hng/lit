

#include <iostream>
#include <boost/asio.hpp>
#include <lit.hpp>
#include <lit/client.h>
#include <gsl/gsl>

namespace net = boost::asio;

int main() {
    auto defer = gsl::finally([] { lit::http_fetch_pool_cleanup(); });
    net::io_context ioc;

    net::co_spawn(ioc, []() -> net::awaitable<void> {
        auto resp = co_await lit::http_get("http://127.0.0.1:8088", true);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- 分割线 -------------------" << std::endl;
        resp = co_await lit::http_get("http://127.0.0.1:8088/api/a");
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- 分割线 -------------------" << std::endl;
        resp = co_await lit::http_get("http://127.0.0.1:8088/api/param?a=1&b=2");
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- 分割线 -------------------" << std::endl;
        resp = co_await lit::http_post("http://127.0.0.1:8088/api/c", "{}", true);
        std::cout << resp.body() << std::endl;

        std::cout << "------------------- 分割线 -------------------" << std::endl;
        std::vector<lit::multipart_formdata_t> form_data = {
            {"field1", "",            "text/plain", "This is the content of field1"     },
            {"file1",  "example.txt", "text/plain", "This is the content of example.txt"},
            {"field2", "",            "text/plain", "This is the content of field2"     }
        };
        resp = co_await lit::http_upload("http://127.0.0.1:8088/api/upload", form_data);
        std::cout << resp.body() << std::endl;
    }, net::detached);

    ioc.run();
    return 0;
}
