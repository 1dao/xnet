// example_http_server.cpp
#include "xhttpd.h"
#include <iostream>
#include <thread>

// GET /api/hello 处理函数
xCoroTaskT<bool> handle_hello(HttpRequest* req, HttpResponse* /*resp*/) {
    const char* name = "World";
    size_t name_len = 0;
    const char* param = xhttpd_get_query_param(req, "name", &name_len);

    if (param && name_len > 0) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%.*s", (int)name_len, param);
        name = buffer;
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{\"message\": \"Hello, %s!\", \"timestamp\": %llu}",
        name, (unsigned long long)time(NULL));

    xhttpd_send_json(req->channel, 200, json);
    co_return true;
}

// POST /api/echo 处理函数
xCoroTaskT<bool> handle_echo(HttpRequest* req, HttpResponse* resp) {
    xhttpd_set_header(resp, "Content-Type", "application/json");

    if (req->body_len > 0) {
        // 返回接收到的数据
        char* json = (char*)malloc(req->body_len + 100);
        if (json) {
            snprintf(json, req->body_len + 100,
                "{\"received\": %.*s, \"length\": %zu}",
                (int)req->body_len, req->body, req->body_len);
            resp->body = json;
            resp->body_len = strlen(json);
        }
    }

    xhttpd_send_response(req->channel, resp);
    co_return true;
}

// 主函数
int main() {
    aeEventLoop* el = aeCreateEventLoop(1024);
    if (!el) {
        xlog_err("Failed to create event loop");
        return -1;
    }
    xlog_init(XLOG_DEBUG, true, true, "logs/xlog.log");
    xlog_set_show_thread_name(true);
    coroutine_init();

    // 初始化HTTP服务器
    if (!xhttpd_init()) {
        std::cerr << "Failed to initialize HTTP server" << std::endl;
        return 1;
    }

    // 配置服务器
    HttpServerConfig config = {
        .port = 8080,
        .host = "0.0.0.0",
        .max_connections = 1000,
        .request_timeout_ms = 30000,
        .max_body_size = 10 * 1024 * 1024,
        .enable_cors = true,
        .cors_origin = "*"
    };

    // 注册路由
    xhttpd_register_route(HTTP_GET, "/api/hello", handle_hello, NULL);
    xhttpd_register_route(HTTP_POST, "/api/echo", handle_echo, NULL);
    xhttpd_register_route(HTTP_GET, "/api/status", [](HttpRequest* req, HttpResponse* /*resp*/) -> xCoroTaskT<bool> {
        char json[256];
        snprintf(json, sizeof(json),
            "{\"status\": \"OK\", \"connections\": %zu, \"requests\": %llu}",
            xhttpd_get_active_connections(),
            (unsigned long long)xhttpd_get_total_requests());
        xhttpd_send_json(req->channel, 200, json);
        co_return true;
        }, NULL);

    // 启动服务器
    if (!xhttpd_start(&config)) {
        std::cerr << "Failed to start HTTP server" << std::endl;
        xhttpd_uninit();
        return 1;
    }

    std::cout << "HTTP server started on http://" << config.host << ":" << config.port << std::endl;
    std::cout << "Available endpoints:" << std::endl;
    std::cout << "  GET  /api/hello" << std::endl;
    std::cout << "  POST /api/echo" << std::endl;
    std::cout << "  GET  /api/status" << std::endl;

    while (true) {
        aeProcessEvents(el, AE_ALL_EVENTS);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 清理
    xhttpd_stop();
    xhttpd_uninit();

    coroutine_uninit();
    xlog_uninit();
    aeDeleteEventLoop(el);
    return 0;
}