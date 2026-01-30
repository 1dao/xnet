#include "xhttpd.h"
#include "xchannel.h"
#include "xchannel.inl"
#include "xcoroutine.h"
#include "xpack.h"
#include "xlog.h"
#include "3rd/picohttpparser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const HttpServerConfig DEFAULT_CONFIG = {
    .port = 8080,
    .host = "0.0.0.0",
    .max_connections = 1024,
    .request_timeout_ms = 30000,
    .max_body_size = 10 * 1024 * 1024, // 10MB
    .enable_cors = false,
    .cors_origin = "*"
};

typedef struct {
    HttpRequest request;
    HttpResponse response;
    char* raw_request;      // 原始请求数据
    size_t raw_len;         // 原始数据长度
    size_t parsed_len;      // 已解析长度
    bool is_parsing;        // 是否正在解析
    bool is_chunked;        // 是否是分块传输
    size_t content_length;  // 内容长度
    uint64_t request_time;  // 请求时间戳
    struct phr_chunked_decoder chunk_decoder;

    int coro_id;                       // corount id
    struct phr_header* parsed_headers; // 解析后的头部信息
    size_t parsed_headers_count;
} HttpConnection;

typedef struct {
    HttpRoute* routes;
    size_t count;
    size_t capacity;
} RouteTable;

typedef struct {
    RouteTable route_table;
    HttpServerConfig config;
    aeEventLoop* event_loop;
    bool is_running;
    size_t active_connections;
    uint64_t total_requests;
    HttpHandler default_404_handler;
    HttpHandler default_500_handler;
} HttpServerState;

static HttpServerState _httpd_state = {};

static int on_http_data(xChannel* channel, char* buf, int len);
static int on_http_closed(xChannel* channel, char* buf, int len);
static xCoroTask process_http_request(void* conn);
static const HttpRoute* find_route(HttpMethod method, const char* path, size_t path_len);
static void free_connection(HttpConnection* conn);
static HttpConnection* create_connection(xChannel* channel);
static void reset_connection(HttpConnection* conn);
static bool parse_http_request(HttpConnection* conn);
static void build_default_response(HttpResponse* resp, int status_code);
static const char* get_method_string(HttpMethod method);
static const char* get_status_text(int status_code);
static const char* get_post_field(const HttpRequest* req, const char* field_name, size_t* value_len);
static xCoroTaskT<bool> xhttpd_default_404_handler(HttpRequest* req, HttpResponse* resp);
static xCoroTaskT<bool> xhttpd_default_500_handler(HttpRequest* req, HttpResponse* resp);

bool xhttpd_init() {
    if (_httpd_state.is_running) {
        xlog_warn("HTTP server already initialized");
        return true;
    }

    _httpd_state.route_table.capacity = 32;
    _httpd_state.route_table.routes = (HttpRoute*)malloc(
        sizeof(HttpRoute) * _httpd_state.route_table.capacity);
    if (!_httpd_state.route_table.routes) {
        xlog_err("Failed to allocate route table");
        return false;
    }
    _httpd_state.route_table.count = 0;
    _httpd_state.config = DEFAULT_CONFIG;

    _httpd_state.default_404_handler = xhttpd_default_404_handler;
    _httpd_state.default_500_handler = xhttpd_default_500_handler;

    _httpd_state.is_running = false;
    _httpd_state.active_connections = 0;
    _httpd_state.total_requests = 0;

    xlog_info("HTTP server initialized");
    return true;
}

// 销毁HTTP服务器
void xhttpd_uninit() {
    if (_httpd_state.route_table.routes) {
        // 清理路由用户数据
        for (size_t i = 0; i < _httpd_state.route_table.count; i++) {
            if (_httpd_state.route_table.routes[i].userdata) {
                free(_httpd_state.route_table.routes[i].userdata);
            }
        }
        free(_httpd_state.route_table.routes);
        _httpd_state.route_table.routes = NULL;
    }

    _httpd_state.route_table.count = 0;
    _httpd_state.route_table.capacity = 0;

    xlog_info("HTTP server uninitialized");
}

bool xhttpd_start(const HttpServerConfig* config) {
    if (_httpd_state.is_running) {
        xlog_warn("HTTP server already running");
        return true;
    }

    if (config) {
        _httpd_state.config = *config;
    }

    if (xchannel_listen(_httpd_state.config.port,
        (char*)_httpd_state.config.host,
        on_http_data,
        on_http_closed,
        NULL,
        xrpoto_crlf_http1) == AE_ERR) {
        xlog_err("Failed to start HTTP server on %s:%d",
            _httpd_state.config.host, _httpd_state.config.port);
        return false;
    }

    _httpd_state.is_running = true;
    xlog_info("HTTP server started on http://%s:%d",
        _httpd_state.config.host, _httpd_state.config.port);

    return true;
}

void xhttpd_stop() {
    if (!_httpd_state.is_running) {
        return;
    }

    _httpd_state.is_running = false;
    xlog_info("HTTP server stopped");
}

bool xhttpd_register_route(HttpMethod method, const char* path,
    HttpHandler handler, void* userdata) {
    if (!path || !handler) {
        xlog_err("Invalid route parameters");
        return false;
    }

    for (size_t i = 0; i < _httpd_state.route_table.count; i++) {
        if (_httpd_state.route_table.routes[i].method == method &&
            strcmp(_httpd_state.route_table.routes[i].path_pattern, path) == 0) {
            xlog_warn("Route already registered: %s %s",
                get_method_string(method), path);
            return false;
        }
    }

    if (_httpd_state.route_table.count >= _httpd_state.route_table.capacity) {
        size_t new_capacity = _httpd_state.route_table.capacity * 2;
        HttpRoute* new_routes = (HttpRoute*)realloc(
            _httpd_state.route_table.routes, sizeof(HttpRoute) * new_capacity);
        if (!new_routes) {
            xlog_err("Failed to expand route table");
            return false;
        }
        _httpd_state.route_table.routes = new_routes;
        _httpd_state.route_table.capacity = new_capacity;
    }

    HttpRoute* route = &_httpd_state.route_table.routes[_httpd_state.route_table.count++];
    route->method = method;
    route->path_pattern = strdup(path);
    route->handler = handler;
    route->userdata = userdata;

    xlog_info("Registered route: %s %s", get_method_string(method), path);
    return true;
}

bool xhttpd_register_routes(const HttpRoute* routes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!xhttpd_register_route(routes[i].method, routes[i].path_pattern,
            routes[i].handler, routes[i].userdata)) {
            return false;
        }
    }
    return true;
}

static int on_http_data(xChannel* channel, char* buf, int len) {
    HttpConnection* conn = (HttpConnection*)channel->userdata;

    if (!conn) {
        conn = create_connection(channel);
        if (!conn) {
            xlog_err("Failed to create HTTP connection context");
            return -1;
        }
        channel->userdata = conn;
    }

    if (conn->raw_len + len > _httpd_state.config.max_body_size) {
        xlog_err("Request too large, max: %zu", _httpd_state.config.max_body_size);
        xhttpd_send_error(channel, 413, "Request Entity Too Large");
        free_connection(conn);
        channel->userdata = NULL;
        return -1;
    }

    char* new_raw = (char*)realloc(conn->raw_request, conn->raw_len + len);
    if (!new_raw) {
        xlog_err("Failed to allocate request buffer");
        free_connection(conn);
        channel->userdata = NULL;
        return -1;
    }

    conn->raw_request = new_raw;
    memcpy(conn->raw_request + conn->raw_len, buf, len);
    conn->raw_len += len;
    if (conn->parsed_headers) // processing
        return len;
    if (!parse_http_request(conn)) {
        return len; // need more data
    }

    coroutine_run(process_http_request, conn);
    return len;
}

static int on_http_closed(xChannel* channel, char* /*buf*/, int /*len*/) {
    HttpConnection* conn = (HttpConnection*)channel->userdata;
    if (conn) {
        free_connection(conn);
        channel->userdata = NULL;
    }

    if (_httpd_state.active_connections > 0) {
        _httpd_state.active_connections--;
    }

    xlog_debug("HTTP connection closed, fd: %d, active: %zu",
        channel->fd, _httpd_state.active_connections);
    return 0;
}

static xCoroTask process_http_request(void* arg) {
    HttpConnection* conn = (HttpConnection*)arg;
    _httpd_state.total_requests++;
    conn->coro_id = coroutine_self_id();

    const HttpRoute* route = find_route(conn->request.method,
        conn->request.path,
        conn->request.path_len);
    build_default_response(&conn->response, 200);

    if (_httpd_state.config.enable_cors) { // 设置CORS头（如果启用）
        xhttpd_set_header(&conn->response, "Access-Control-Allow-Origin",
            _httpd_state.config.cors_origin);
        xhttpd_set_header(&conn->response, "Access-Control-Allow-Methods",
            "GET, POST, PUT, DELETE, OPTIONS");
        xhttpd_set_header(&conn->response, "Access-Control-Allow-Headers",
            "Content-Type, Authorization");
    }

    try {
        if (route) {
            auto ret = co_await route->handler(&conn->request, &conn->response);
            xlog_info("HTTP request handled, pt=%.*s, status: %d", conn->request.path_len, conn->request.path, ret?200:500);
        } else {
            _httpd_state.default_404_handler(&conn->request, &conn->response);
        }
    }
    catch (const std::exception& e) {
        xlog_err("Exception in HTTP handler: %s", e.what());
        _httpd_state.default_500_handler(&conn->request, &conn->response);
    }
    catch (...) {
        xlog_err("Unknown exception in HTTP handler");
        _httpd_state.default_500_handler(&conn->request, &conn->response);
    }

    // 发送响应- 减少不必要的堆分配，直接在协议中send
    // xhttpd_send_response(conn->request.channel, &conn->response);

    // 如果保持长连接，清理上一请求的临时状态并尝试处理缓冲区中可能存在的下一个请求
    reset_connection(conn);
    if (conn->response.keep_alive) {
        if (conn->raw_len > 0) {
            // 如果缓冲区中已有完整的下一请求，立即解析并重新派发处理
            if (parse_http_request(conn)) {
                coroutine_run(process_http_request, conn);
                co_return; // 当前协程完成，后续处理由新协程接管
            }
        }
    } else {
        co_await coroutine_sleep(_httpd_state.config.request_timeout_ms);
        if (coroutine_valid(0))
            xchannel_close(conn->request.channel);
    }

    co_return;
}

static const HttpRoute* find_route(HttpMethod method, const char* path, size_t path_len) {
    for (size_t i = 0; i < _httpd_state.route_table.count; i++) {
        HttpRoute* route = &_httpd_state.route_table.routes[i];

        if (route->method != method) {
            continue;
        }

        // 简单路径匹配（TODO: 支持通配符和参数）
        if (strncmp(route->path_pattern, path, path_len) == 0 &&
            route->path_pattern[path_len] == '\0') {
            return route;
        }
    }
    return NULL;
}

static HttpConnection* create_connection(xChannel* channel) {
    HttpConnection* conn = (HttpConnection*)calloc(1, sizeof(HttpConnection));
    if (!conn) {
        return NULL;
    }

    conn->request.channel = channel;
    memset(&conn->chunk_decoder, 0, sizeof(conn->chunk_decoder));
    conn->response.headers.capacity = 8;
    conn->response.headers.names = (const char**)malloc(sizeof(char*) * conn->response.headers.capacity);
    conn->response.headers.values = (const char**)malloc(sizeof(char*) * conn->response.headers.capacity);
    conn->response.headers.name_lens = (size_t*)malloc(sizeof(size_t) * conn->response.headers.capacity);
    conn->response.headers.value_lens = (size_t*)malloc(sizeof(size_t) * conn->response.headers.capacity);

    if (!conn->response.headers.names || !conn->response.headers.values ||
        !conn->response.headers.name_lens || !conn->response.headers.value_lens) {
        free(conn);
        return NULL;
    }

    _httpd_state.active_connections++;
    xlog_debug("New HTTP connection, fd: %d, active: %zu",
        channel->fd, _httpd_state.active_connections);

    return conn;
}

static void free_connection(HttpConnection* conn) {
    if (!conn) return;

    if (conn->raw_request) {
        free(conn->raw_request);
    }
    if (conn->parsed_headers) {
        free(conn->parsed_headers);
    }
    if (conn->response.headers.names) {
        free((void*)conn->response.headers.names);
    }
    if (conn->response.headers.values) {
        free((void*)conn->response.headers.values);
    }
    if (conn->response.headers.name_lens) {
        free(conn->response.headers.name_lens);
    }
    if (conn->response.headers.value_lens) {
        free(conn->response.headers.value_lens);
    }
    if (conn->coro_id != 0) {
        int coro_id = conn->coro_id;
        conn->coro_id = 0;
        coroutine_cancel(coro_id);
    }
}

// 重置连接状态以准备接收下一条请求（不关闭连接）
static void reset_connection(HttpConnection* conn) {
    if (!conn) return;

    // 释放解析头
    if (conn->parsed_headers) {
        free(conn->parsed_headers);
        conn->parsed_headers = NULL;
        conn->parsed_headers_count = 0;
    }

    // 计算已消耗的数据长度（请求头 + 请求体）
    size_t consumed = conn->parsed_len;
    if (conn->content_length > 0) consumed += conn->content_length;

    // 如果是 chunked，parsed_len 已指向 body 起始，body_len 在 request.body_len
    if (conn->is_chunked && conn->request.body_len > 0) {
        consumed = conn->parsed_len + conn->request.body_len;
    }

    // 移动剩余数据到缓冲头部
    if (conn->raw_request) {
        if (conn->raw_len > consumed) {
            size_t remaining = conn->raw_len - consumed;
            memmove(conn->raw_request, conn->raw_request + consumed, remaining);
            conn->raw_len = remaining;
        } else {
            free(conn->raw_request);
            conn->raw_request = NULL;
            conn->raw_len = 0;
        }
    }

    // 重置解析和请求相关字段
    conn->parsed_len = 0;
    conn->is_parsing = false;
    conn->is_chunked = false;
    conn->content_length = 0;

    conn->request.path = NULL;
    conn->request.path_len = 0;
    conn->request.query_string = NULL;
    conn->request.query_len = 0;
    conn->request.headers = NULL;
    conn->request.num_headers = 0;
    conn->request.body = NULL;
    conn->request.body_len = 0;

    // 清理 response（保持 headers buffers 可重复使用，但清空计数）
    conn->response.body = NULL;
    conn->response.body_len = 0;
    conn->response.headers.count = 0;

    // 重置 chunk 解码器
    memset(&conn->chunk_decoder, 0, sizeof(conn->chunk_decoder));
}

static bool parse_http_request(HttpConnection* conn) {
    if (conn->is_parsing) {
        return false;
    }
    conn->is_parsing = true;

    const char* method;
    size_t method_len;
    const char* path;
    size_t path_len;
    int minor_version;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);
    int pret;

    if (conn->parsed_headers) {
        free(conn->parsed_headers);
        conn->parsed_headers = NULL;
        conn->parsed_headers_count = 0;
    }
    
    pret = phr_parse_request(conn->raw_request, conn->raw_len,
        &method, &method_len,
        &path, &path_len,
        &minor_version,
        headers, &num_headers, 0);

    if (pret == -1) {
        xlog_err("Failed to parse HTTP request");
        conn->is_parsing = false;
        return false;
    } else if (pret == -2) {
        conn->is_parsing = false;
        return false;
    }

    conn->parsed_headers = (struct phr_header*)malloc(sizeof(struct phr_header) * num_headers);
    if (!conn->parsed_headers) {
        conn->is_parsing = false;
        return false;
    }
    memcpy(conn->parsed_headers, headers, sizeof(struct phr_header) * num_headers);
    conn->parsed_headers_count = num_headers;
    conn->parsed_len = pret; // 解析成功

    // 填充请求结构
    if (method_len == 3 && strncmp(method, "GET", 3) == 0) {
        conn->request.method = HTTP_GET;
    } else if (method_len == 4 && strncmp(method, "POST", 4) == 0) {
        conn->request.method = HTTP_POST;
    } else if (method_len == 3 && strncmp(method, "PUT", 3) == 0) {
        conn->request.method = HTTP_PUT;
    } else if (method_len == 6 && strncmp(method, "DELETE", 6) == 0) {
        conn->request.method = HTTP_DELETE;
    } else {
        conn->request.method = HTTP_GET; // 默认
    }
    conn->request.version = minor_version == 0 ? HTTP_VERSION_1_0 : HTTP_VERSION_1_1;// 解析HTTP版本

    const char* query_start = (const char*)memchr(path, '?', path_len); // 解析路径和查询字符串
    if (query_start) {
        conn->request.path_len = query_start - path;
        conn->request.path = path;
        conn->request.query_len = path_len - (conn->request.path_len + 1);
        conn->request.query_string = query_start + 1;
    } else {
        conn->request.path_len = path_len;
        conn->request.path = path;
        conn->request.query_len = 0;
        conn->request.query_string = NULL;
    }
    conn->request.headers = conn->parsed_headers; // parse headers
    conn->request.num_headers = conn->parsed_headers_count;

    conn->content_length = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (conn->content_length==0 && strncasecmp(headers[i].name, "Content-Length", headers[i].name_len) == 0) {
            conn->content_length = atoll(headers[i].value);
            break;
        }
    }

    if (conn->content_length > 0 &&
        conn->raw_len >= conn->parsed_len + conn->content_length) { // 检查是否有请求体
        conn->request.body = conn->raw_request + conn->parsed_len;
        conn->request.body_len = conn->content_length;
        conn->is_parsing = false;
        return true;
    }

    for (size_t i = 0; i < num_headers; i++) { // 检查Transfer-Encoding: chunked
        if (strncasecmp(headers[i].name, "Transfer-Encoding", headers[i].name_len) == 0 &&
            strncasecmp(headers[i].value, "chunked", headers[i].value_len) == 0) {
            conn->is_chunked = true;
            break;
        }
    }

    if (conn->is_chunked) { // 处理分块传输
        size_t body_len = conn->raw_len - conn->parsed_len;
        ssize_t r = phr_decode_chunked(&conn->chunk_decoder,
            conn->raw_request + conn->parsed_len,
            &body_len);
        if (r == -1) {
            xlog_err("Failed to decode chunked data");
            conn->is_parsing = false;
            return false;
        } else if (r == -2) { // 需要更多数据
            conn->is_parsing = false;
            return false;
        } else {
            conn->request.body = conn->raw_request + conn->parsed_len;
            conn->request.body_len = body_len;
            conn->is_parsing = false;
            return true;
        }
    }

    // 没有请求体或请求体已完整
    conn->request.body = NULL;
    conn->request.body_len = 0;
    conn->is_parsing = false;
    return true;
}

static void build_default_response(HttpResponse* resp, int status_code) {
    resp->status_code = status_code;
    resp->status_text = get_status_text(status_code);
    resp->body = NULL;
    resp->body_len = 0;
    resp->keep_alive = false;
    resp->headers.count = 0;

    xhttpd_set_header(resp, "Server", "xhttpd/1.0");
    xhttpd_set_header(resp, "Connection", "keep-alive");
}

int xhttpd_send_response(xChannel* channel, const HttpResponse* resp) {
    if (!channel || !resp) {
        return -1;
    }

    char status_line[128]; // 构建响应头
    int len = snprintf(status_line, sizeof(status_line),
        "HTTP/1.1 %d %s\r\n",
        resp->status_code, resp->status_text);
    if (xchannel_sbuf(channel, status_line, len) != len) {
        return -1;
    }

    for (size_t i = 0; i < resp->headers.count; i++) { // 发送头部
        char header_line[1024];
        len = snprintf(header_line, sizeof(header_line),
            "%.*s: %.*s\r\n",
            (int)resp->headers.name_lens[i], resp->headers.names[i],
            (int)resp->headers.value_lens[i], resp->headers.values[i]);

        if (xchannel_sbuf(channel, header_line, len) != len) {
            return -1;
        }
    }

    if (xchannel_sbuf(channel, "\r\n", 2) != 2) {
        return -1;
    }

    if (resp->body && resp->body_len > 0) { // 发送响应体
        if (xchannel_sbuf(channel, resp->body, resp->body_len) != (int)resp->body_len) {
            return -1;
        }
    }
    xchannel_flush(channel);
    return 0;
}

int xhttpd_send_text(xChannel* channel, int status_code, const char* text) { // 发送简单文本响应
    HttpConnection* conn = (HttpConnection*)channel->userdata;
    if (!conn) {
        xlog_err("No connection context for channel");
        return -1;
    }

    HttpResponse* resp = &conn->response;
    build_default_response(resp, status_code);
    xhttpd_set_header(resp, "Content-Type", "text/plain; charset=utf-8");

    if (text) {
        resp->body = text;
        resp->body_len = strlen(text);
        char content_len[32];
        snprintf(content_len, sizeof(content_len), "%zu", resp->body_len);
        xhttpd_set_header(resp, "Content-Length", content_len);
    }

    return xhttpd_send_response(channel, resp);
}

int xhttpd_send_json(xChannel* channel, int status_code, const char* json) {
    HttpConnection* conn = (HttpConnection*)channel->userdata;
    if (!conn) {
        xlog_err("No connection context for channel");
        return -1;
    }

    HttpResponse* resp = &conn->response;

    build_default_response(resp, status_code);
    xhttpd_set_header(resp, "Content-Type", "application/json; charset=utf-8");

    if (json) {
        resp->body = json;
        resp->body_len = strlen(json);
        char content_len[32];
        snprintf(content_len, sizeof(content_len), "%zu", resp->body_len);
        xhttpd_set_header(resp, "Content-Length", content_len);
    }

    return xhttpd_send_response(channel, resp);
}

static inline std::string escape_json_string(const char* str) {
    if (!str) return "";
    
    std::string result;
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                // 检查是否是非ASCII字符
                if ((unsigned char)*p < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
                    result += buf;
                } else {
                    result += *p;
                }
                break;
        }
    }
    return result;
}

int xhttpd_send_error(xChannel* channel, int status_code, const char* message) {
    std::string msg = escape_json_string(message ? message : get_status_text(status_code));
    char error_json[512];
    snprintf(error_json, sizeof(error_json),
        "{\"error\": {\"code\": %d, \"message\": \"%s\"}}",
        status_code, msg.c_str());
    
    return xhttpd_send_json(channel, status_code, error_json);
}

HttpResponse* xhttpd_get_response(xChannel* channel) {
    HttpConnection* conn = (HttpConnection*)channel->userdata;
    return conn ? &conn->response : NULL;
}

const char* xhttpd_get_query_param(const HttpRequest* req, const char* key, size_t* value_len) {
    if (!req || !key) {
        return NULL;
    }

    HttpMethod method = req->method;
    if (method == HTTP_POST) {
        const char* result = get_post_field(req, key, value_len);
        if (result)
            return result;
    } else {
        if (!req->query_string)return NULL;

        size_t key_len = strlen(key);
        const char* query = req->query_string;
        size_t query_len = req->query_len;

        while (query_len > 0) {
            const char* eq = (const char*)memchr(query, '=', query_len);
            if (!eq) break;

            size_t param_key_len = eq - query;

            // 查找参数值结束位置
            const char* value_end = (const char*)memchr(eq + 1, '&', query_len - (eq - query + 1));
            size_t param_value_len;
            if (value_end) {
                param_value_len = value_end - eq - 1;
            } else {
                param_value_len = query_len - (eq - query + 1);
            }

            // 检查键是否匹配
            if (param_key_len == key_len && strncmp(query, key, key_len) == 0) {
                if (value_len) *value_len = param_value_len;
                return eq + 1;
            }

            // 移动到下一个参数
            if (value_end) {
                size_t skip = value_end - query + 1;
                query += skip;
                query_len -= skip;
            }
            else {
                break;
            }
        }
    }
    return NULL;
}

const char* xhttpd_get_header(const HttpRequest* req, const char* name, size_t* value_len) {
    if (!req || !name) {
        return NULL;
    }

    size_t name_len = strlen(name);
    for (size_t i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == name_len &&
            strncasecmp(req->headers[i].name, name, name_len) == 0) {
            if (value_len) *value_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }

    return NULL;
}

bool xhttpd_set_header(HttpResponse* resp, const char* name, const char* value) {
    if (!resp || !name || !value) {
        return false;
    }

    // 检查是否需要扩容
    if (resp->headers.count >= resp->headers.capacity) {
        size_t new_capacity = resp->headers.capacity * 2;
        const char** new_names = (const char**)realloc(
            (void*)resp->headers.names, sizeof(char*) * new_capacity);
        const char** new_values = (const char**)realloc(
            (void*)resp->headers.values, sizeof(char*) * new_capacity);
        size_t* new_name_lens = (size_t*)realloc(
            resp->headers.name_lens, sizeof(size_t) * new_capacity);
        size_t* new_value_lens = (size_t*)realloc(
            resp->headers.value_lens, sizeof(size_t) * new_capacity);

        if (!new_names || !new_values || !new_name_lens || !new_value_lens) {
            return false;
        }

        resp->headers.names = new_names;
        resp->headers.values = new_values;
        resp->headers.name_lens = new_name_lens;
        resp->headers.value_lens = new_value_lens;
        resp->headers.capacity = new_capacity;
    }

    // 添加头部
    resp->headers.names[resp->headers.count] = name;
    resp->headers.values[resp->headers.count] = value;
    resp->headers.name_lens[resp->headers.count] = strlen(name);
    resp->headers.value_lens[resp->headers.count] = strlen(value);
    resp->headers.count++;

    return true;
}

bool xhttpd_set_body(HttpResponse* resp, const char* body, size_t body_len) {
    if (body_len <= 0 || !body) return false;
    if (body_len > _httpd_state.config.max_body_size) {
        xlog_err("Body size exceeds limit...");
        return false;
    }

    char content_len[64];
    snprintf(content_len, sizeof(content_len), "%zu", body_len);
    xhttpd_set_header((HttpResponse*)resp, "Content-Length", content_len);

    resp->body = body;
    resp->body_len = body_len;
    return true;
}

size_t xhttpd_get_active_connections() {
    return _httpd_state.active_connections;
}

uint64_t xhttpd_get_total_requests() {
    return _httpd_state.total_requests;
}

static xCoroTaskT<bool> xhttpd_default_404_handler(HttpRequest* req, HttpResponse* resp) {
    resp->status_code = 404;
    resp->status_text = "Not Found";
    xhttpd_set_header(resp, "Content-Type", "application/json");

    char json[256];
    snprintf(json, sizeof(json),
        "{\"error\": {\"code\": 404, \"message\": \"Path '%s' not found\"}}",
        req->path);
    resp->body = json;
    resp->body_len = strlen(json);
    xhttpd_send_response(req->channel, resp);
    co_return true;
}

static xCoroTaskT<bool> xhttpd_default_500_handler(HttpRequest* req, HttpResponse* resp) {
    resp->status_code = 500;
    resp->status_text = "Internal Server Error";
    xhttpd_set_header(resp, "Content-Type", "application/json");

    const char* json = "{\"error\": {\"code\": 500, \"message\": \"Internal server error\"}}";
    resp->body = json;
    resp->body_len = strlen(json);
    xhttpd_send_response(req->channel, resp);
    co_return true;
}

static const char* get_method_string(HttpMethod method) {
    static const char* method_strings[] = {
        "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"
    };
    return method_strings[method];
}

static const char* get_status_text(int status_code) {
    switch (status_code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Request Entity Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

static inline void* memmem_raw(const void* haystack, size_t haystack_len,
    const void* needle, size_t needle_len) {
    if (needle_len == 0) return (void*)haystack;
    if (haystack_len < needle_len) return NULL;

    const char* h = (const char*)haystack;
    const char* n = (const char*)needle;

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(h + i, n, needle_len) == 0) {
            return (void*)(h + i);
        }
    }
    return NULL;
}

static inline bool fast_memequal(const void* a, const void* b, size_t len) {
    if (len == 0) return true;
    
    const char* pa = (const char*)a;
    const char* pb = (const char*)b;
    while (len >= 8) {
        if (*(const uint64_t*)pa != *(const uint64_t*)pb) {
            return false;
        }
        pa += 8;
        pb += 8;
        len -= 8;
    }
    
    if (len >= 4) {
        if (*(const uint32_t*)pa != *(const uint32_t*)pb) {
            return false;
        }
        pa += 4;
        pb += 4;
        len -= 4;
    }
    
    if (len >= 2) {
        if (*(const uint16_t*)pa != *(const uint16_t*)pb) {
            return false;
        }
        pa += 2;
        pb += 2;
        len -= 2;
    }
    
    if (len >= 1) {
        if (*pa != *pb) {
            return false;
        }
    }
    
    return true;
}

static inline const void* fast_memsearch(const void* haystack, size_t haystack_len,
                                 const void* needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (haystack_len < needle_len) return NULL;
    
    const char* hs = (const char*)haystack;
    const char* ndl = (const char*)needle;
    
    if (needle_len <= 16) {
        const char* end = hs + haystack_len - needle_len + 1;
        if (needle_len >= 8) { // 如果模式长度>=8，先按8字节比较
            uint64_t needle_word = *(const uint64_t*)ndl;
            for (const char* p = hs; p < end; p++) {
                if (*(const uint64_t*)p == needle_word) {
                    if (fast_memequal(p + 8, ndl + 8, needle_len - 8)) {
                        return p;
                    }
                }
            }
        } else if (needle_len >= 4) {
            uint32_t needle_dword = *(const uint32_t*)ndl;
            for (const char* p = hs; p < end; p++) {
                if (*(const uint32_t*)p == needle_dword) {
                    if (fast_memequal(p + 4, ndl + 4, needle_len - 4)) {
                        return p;
                    }
                }
            }
        } else { // 对于更短的模式，直接逐字节比较
            for (const char* p = hs; p < end; p++) {
                if (fast_memequal(p, ndl, needle_len)) {
                    return p;
                }
            }
        }
        return NULL;
    }
    return memmem_raw(haystack, haystack_len, needle, needle_len);
}

static const void* memmem_(const void* haystack, size_t haystack_len,
                                    const void* needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (haystack_len < needle_len) return NULL;
    
    const char* hs = (const char*)haystack;
    const char* ndl = (const char*)needle;
    const char* end = hs + haystack_len - needle_len + 1;
    if (needle_len <= 8) {
        return fast_memsearch(haystack, haystack_len, needle, needle_len);
    }
   
    size_t blocks_64 = needle_len / 8;
    size_t remainder = needle_len % 8;
    const uint64_t* needle_blocks = (const uint64_t*)ndl;
    
    uint64_t first_block = needle_blocks[0];
    for (const char* p = hs; p < end; p++) {
        if (*(const uint64_t*)p == first_block) {
            bool match = true;
            for (size_t i = 1; i < blocks_64; i++) {
                if (*(const uint64_t*)(p + i * 8) != needle_blocks[i]) {
                    match = false;
                    break;
                }
            }
            
            if (match && remainder > 0) {
                const char* remainder_start = p + blocks_64 * 8;
                const char* needle_remainder = ndl + blocks_64 * 8;
                if (!fast_memequal(remainder_start, needle_remainder, remainder)) {
                    match = false;
                }
            }
            if (match)
                return p;
        }
    }
    
    return NULL;
}

// 从multipart/form-data中提取boundary
static const char* get_multipart_boundary(const HttpRequest* req) {
    // 查找Content-Type头部
    for (size_t i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == 12 &&
            strncasecmp(req->headers[i].name, "Content-Type", 12) == 0) {

            const char* content_type = req->headers[i].value;
            size_t ct_len = req->headers[i].value_len;

            // 查找boundary=
            const char* boundary_ptr = (const char*)memmem_(content_type, ct_len,
                "boundary=", 9);
            if (boundary_ptr) {
                // 找到boundary，返回指向boundary值的指针
                return boundary_ptr + 9; // 跳过"boundary="
            }
            break;
        }
    }
    return NULL;
}

// 从Content-Disposition中提取字段名
static char* extract_field_name(const char* header, size_t header_len, size_t* name_len) {
    const char* name_start = (const char*)memmem_(header, header_len, "name=\"", 6);
    if (!name_start) return NULL;

    name_start += 6;
    const char* name_end = (const char*)memmem_(name_start,
        header + header_len - name_start,
        "\"", 1);
    if (!name_end) return NULL;

    *name_len = name_end - name_start;
    char* name = (char*)malloc(*name_len + 1);
    if (name) {
        memcpy(name, name_start, *name_len);
        name[*name_len] = '\0';
    }
    return name;
}

// 从multipart数据中获取字段值
static const char* get_multipart_form_field(const HttpRequest* req, const char* field_name,
    size_t* value_len) {
    if (!req || !req->body || !field_name) return NULL;

    const char* boundary = get_multipart_boundary(req);
    if (!boundary) return NULL;

    // 计算boundary长度（直到下一个分号或字符串结尾或空格）
    size_t boundary_len_actual = 0;
    while (boundary[boundary_len_actual] != '\0' &&
        boundary[boundary_len_actual] != ';' &&
        boundary[boundary_len_actual] != ' ' &&
        boundary[boundary_len_actual] != '\r' &&
        boundary[boundary_len_actual] != '\n') {
        boundary_len_actual++;
    }

    // 如果boundary为空，则无法处理
    if (boundary_len_actual == 0) {
        return NULL;
    }

    // 构造边界字符串
    char* boundary_line = (char*)malloc(boundary_len_actual + 3); // "--" + boundary
    if (!boundary_line) return NULL;

    sprintf(boundary_line, "--%.*s", (int)boundary_len_actual, boundary);
    size_t boundary_len = strlen(boundary_line);

    const char* pos = req->body;
    size_t remaining = req->body_len;

    // 查找第一个边界
    const char* first_boundary = (const char*)memmem_(pos, remaining,
        boundary_line, boundary_len);
    if (!first_boundary) {
        free(boundary_line);
        return NULL;
    }

    pos = first_boundary + boundary_len;
    remaining = req->body_len - (pos - req->body);

    // 跳过CRLF
    if (remaining >= 2 && pos[0] == '\r' && pos[1] == '\n') {
        pos += 2;
        remaining -= 2;
    }

    while (remaining > 0) {
        // 查找下一个边界
        const char* next_boundary = (const char*)memmem_(pos, remaining,
            boundary_line, boundary_len);
        if (!next_boundary) {
            break;
        }

        // 解析这部分内容
        size_t section_len = next_boundary - pos;
        if (section_len > 4) { // 至少需要"\r\n\r\n"
            // 查找头部和内容的分隔
            const char* header_end = (const char*)memmem_(pos, section_len,
                "\r\n\r\n", 4);
            if (header_end) {
                size_t header_len = header_end - pos;
                const char* content_start = header_end + 4;
                size_t content_len = section_len - (content_start - pos);

                // 检查字段名
                size_t name_len;
                char* extracted_name = extract_field_name(pos, header_len, &name_len);
                if (extracted_name) {
                    if (name_len == strlen(field_name) &&
                        strncmp(extracted_name, field_name, name_len) == 0) {
                        free(extracted_name);
                        free(boundary_line);

                        // 找到了目标字段，去除首尾的空白字符
                        // 去除开头的空白字符
                        while (content_len > 0 &&
                            (content_start[0] == '\r' || content_start[0] == '\n' || 
                             content_start[0] == ' ' || content_start[0] == '\t')) {
                            content_start++;
                            content_len--;
                        }
                        
                        // 去除末尾的空白字符
                        while (content_len > 0 &&
                            (content_start[content_len - 1] == '\r' ||
                                content_start[content_len - 1] == '\n' ||
                                content_start[content_len - 1] == ' ' ||
                                content_start[content_len - 1] == '\t')) {
                            content_len--;
                        }

                        if (value_len) *value_len = content_len;
                        // 注意：这里返回的是指向请求体内部的指针
                        return content_start;
                    }
                    free(extracted_name);
                }
            }
        }

        pos = next_boundary + boundary_len;
        remaining = req->body_len - (pos - req->body);

        // 跳过CRLF
        if (remaining >= 2 && pos[0] == '\r' && pos[1] == '\n') {
            pos += 2;
            remaining -= 2;
        }
    }

    free(boundary_line);
    return NULL;
}

// 从application/x-www-form-urlencoded数据中获取字段值
static const char* get_urlencoded_form_field(const char* body, size_t body_len,
    const char* field_name, size_t* value_len) {
    if (!body || !field_name) return NULL;

    size_t field_name_len = strlen(field_name);
    const char* pos = body;
    size_t remaining = body_len;

    while (remaining > 0) {
        const char* field_start = pos;
        const char* field_end = (const char*)memmem_(field_start, remaining, "=", 1);
        if (!field_end)
            break;

        size_t current_field_name_len = field_end - field_start;
        if (current_field_name_len == field_name_len &&
            strncmp(field_start, field_name, field_name_len) == 0) {

            // 找到匹配的字段名，获取值
            const char* value_start = field_end + 1;
            const char* value_end = (const char*)memmem_(value_start,
                remaining - (value_start - field_start),
                "&", 1);

            if (!value_end)
                value_end = body + body_len; // 到达字符串末尾

            size_t current_value_len = value_end - value_start;
            if (value_len)
                *value_len = current_value_len;

            // 注意：这里返回的是指向请求体内部的指针
            return value_start;
        }

        // 移动到下一个字段
        const char* next_field = (const char*)memmem_(field_end,
            remaining - (field_end - field_start),
            "&", 1);
        if (!next_field) {
            break;
        }

        pos = next_field + 1;
        remaining = body_len - (pos - body);
    }
    return NULL;
}

static const char* get_post_field(const HttpRequest* req, const char* field_name, size_t* value_len) {
    if (!req->body) return NULL;

    const char* content_type = NULL;
    for (size_t i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == 12 &&
            strncasecmp(req->headers[i].name, "Content-Type", 12) == 0) {
            content_type = req->headers[i].value;
            break;
        }
    }

    if (strncmp(content_type, "application/x-www-form-urlencoded", 33) == 0) { // 处理application/x-www-form-urlencoded类型的POST数据
        return get_urlencoded_form_field(req->body, req->body_len, field_name, value_len);
    } else if (memmem_(content_type, 48, "multipart/form-data", 19) != NULL) {
        return get_multipart_form_field(req, field_name, value_len);            // 处理multipart/form-data类型的POST数据
    }
    return NULL;
}

const char* xhttpd_memsearch(const char* mem, size_t mem_len, const char* sub, size_t sub_len) {
    return (const char*)memmem_(mem, mem_len, sub, sub_len);
}
