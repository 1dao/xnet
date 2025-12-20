#ifndef _XHTTPD_H
#define _XHTTPD_H

#include "ae.h"
#include "xchannel.h"
#include "xpack.h"
#include "xcoroutine.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HTTP_GET = 0,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_PATCH,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_METHOD_COUNT
} HttpMethod;

typedef enum {
    HTTP_VERSION_1_0 = 0,
    HTTP_VERSION_1_1,
    HTTP_VERSION_2_0
} HttpVersion;

typedef struct {
    HttpMethod method;
    HttpVersion version;
    const char* path;
    size_t path_len;
    const char* query_string;
    size_t query_len;
    const char* body;
    size_t body_len;

    struct phr_header* headers;
    size_t num_headers;
    char content_type[48];

    xChannel* channel;

    void* userdata;
} HttpRequest;

typedef struct {
    int status_code;
    const char* status_text;

    struct {
        const char** names;
        const char** values;
        size_t* name_lens;
        size_t* value_lens;
        size_t count;
        size_t capacity;
    } headers;

    const char* body;
    size_t body_len;

    bool keep_alive;
} HttpResponse;

typedef xCoroTaskT<bool>(*HttpHandler)(HttpRequest* req, HttpResponse* resp);

typedef struct {
    HttpMethod method;
    const char* path_pattern;
    HttpHandler handler;
    void* userdata;
} HttpRoute;

typedef struct {
    int port;
    const char* host;
    int max_connections;
    int request_timeout_ms;
    size_t max_body_size;
    bool enable_cors;
    const char* cors_origin;
} HttpServerConfig;

bool xhttpd_init();
void xhttpd_uninit();

bool xhttpd_start(const HttpServerConfig* config);
void xhttpd_stop();

bool xhttpd_register_route(HttpMethod method, const char* path, HttpHandler handler, void* userdata);
bool xhttpd_register_routes(const HttpRoute* routes, size_t count);

int xhttpd_send_response(xChannel* channel, const HttpResponse* resp);
int xhttpd_send_text(xChannel* channel, int status_code, const char* text);
int xhttpd_send_json(xChannel* channel, int status_code, const char* json);
int xhttpd_send_error(xChannel* channel, int status_code, const char* message);
HttpResponse* xhttpd_get_response(xChannel* channel);

const char* xhttpd_get_query_param(const HttpRequest* req, const char* key, size_t* value_len);
const char* xhttpd_get_header(const HttpRequest* req, const char* name, size_t* value_len);

bool xhttpd_set_header(HttpResponse* resp, const char* name, const char* value);
bool xhttpd_set_body(HttpResponse* resp, const char* body, size_t body_len);

size_t   xhttpd_get_active_connections();
uint64_t xhttpd_get_total_requests();
const char* xhttpd_memsearch(const char* mem, size_t mem_len, const char* sub, size_t sub_len);

#endif // _XHTTPD_H