#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ae.h"
#include "anet.h"
#include "zmalloc.h"

// 协议处理函数指针类型
typedef int (*ProtocolHandler)(int param1, const char* param2, int param2_len, char* response, int* response_len);

// 协议结构定义
typedef struct {
    uint16_t protocol;
    ProtocolHandler handler;
} ProtocolMapping;

// 连接上下文结构
typedef struct {
    int fd;
    char recv_buffer[64 * 1024];  // 接收缓冲区
    size_t recv_len;              // 当前接收数据长度
    uint32_t expected_len;        // 期望的包长度
    int parsing;                  // 是否正在解析中
} client_context_t;

// 全局协议映射表
static ProtocolMapping protocol_handlers[256];
static int handler_count = 0;

// 注册协议处理函数
void register_protocol_handler(uint16_t protocol, ProtocolHandler handler) {
    if (handler_count < 256) {
        protocol_handlers[handler_count].protocol = protocol;
        protocol_handlers[handler_count].handler = handler;
        handler_count++;
    }
}

// 查找协议处理函数
ProtocolHandler find_protocol_handler(uint16_t protocol) {
    for (int i = 0; i < handler_count; i++) {
        if (protocol_handlers[i].protocol == protocol) {
            return protocol_handlers[i].handler;
        }
    }
    return NULL;
}

// 示例协议处理函数1
int handle_protocol_1(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    printf("处理协议1: param1=%d, param2=%.*s\n", param1, param2_len, param2);
    *response_len = sprintf(response, "协议1处理结果: %d", param1 * 2);
    return 0; // 成功
}

// 示例协议处理函数2
int handle_protocol_2(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    printf("处理协议2: param1=%d, param2长度=%d\n", param1, param2_len);
    *response_len = sprintf(response, "协议2处理结果: %d字节数据", param2_len);
    return 0; // 成功
}

// 解析请求包并处理
void process_request(const char* request, int request_len, char* response, int* response_len) {
    if (request_len < 12) { // 最小请求包长度：4+2+1+1+4=12字节
        *response_len = 0;
        return;
    }

    // 解析请求包头部
    uint32_t pkg_len = *(const uint32_t*)request;
    uint16_t protocol = *(const uint16_t*)(request + 4);
    uint8_t need_return = *(const uint8_t*)(request + 6);
    uint8_t is_request = *(const uint8_t*)(request + 7);
    uint32_t pkg_id = *(const uint32_t*)(request + 8);

    // 验证包长度
    if (pkg_len != request_len) {
        printf("包长度不匹配: %d vs %d\n", pkg_len, request_len);
        *response_len = 0;
        return;
    }

    // 检查是否为请求包
    if (is_request != 1) {
        printf("不是请求包\n");
        *response_len = 0;
        return;
    }

    // 解析参数
    int param1 = 0;
    const char* param2 = NULL;
    int param2_len = 0;

    if (request_len > 12) {
        param1 = *(const int*)(request + 12);
        if (request_len > 16) {
            param2 = request + 16;
            param2_len = request_len - 16;
        }
    }

    // 查找并调用对应的协议处理函数
    ProtocolHandler handler = find_protocol_handler(protocol);
    char handler_response[1024] = { 0 };
    int handler_response_len = 0;
    int ret = -1;

    if (handler) {
        ret = handler(param1, param2, param2_len, handler_response, &handler_response_len);
    }
    else {
        printf("未找到协议%d的处理函数\n", protocol);
        *response_len = 0;
        return;
    }

    // 构建响应包
    if (need_return) {
        // 响应包头部长度：4+2+1+1+4=12字节
        *response_len = 12 + handler_response_len;
        uint32_t resp_pkg_len = *response_len;
        uint8_t return_flag = 0; // 不返回
        uint8_t is_response = 0; // 返回包

        memcpy(response, &resp_pkg_len, 4);
        memcpy(response + 4, &protocol, 2);
        memcpy(response + 6, &return_flag, 1);
        memcpy(response + 7, &is_response, 1);
        memcpy(response + 8, &pkg_id, 4);
        memcpy(response + 12, handler_response, handler_response_len);
    }
    else {
        *response_len = 0;
    }
}

void client_close(client_context_t* ctx) {
    if (!ctx) return;

    printf("清理客户端连接: fd=%d\n", ctx->fd);

    aeEventLoop* el = aeGetCurEventLoop(); // 需要根据你的实际事件循环获取方式调整
    if (el) {
        aeDeleteFileEvent(el, ctx->fd, AE_READABLE);
        aeDeleteFileEvent(el, ctx->fd, AE_WRITABLE);
    }

    if (ctx->fd != -1) {
        anetCloseSocket(ctx->fd);
        ctx->fd = -1;
    }

    zfree(ctx);
}


// 客户端数据读取处理函数
void read_handler(aeEventLoop* el, int fd, void* privdata, int mask) {
    client_context_t* ctx = (client_context_t*)privdata;

    // 读取数据到缓冲区
    int nread = anetRead(fd, ctx->recv_buffer + ctx->recv_len,
        sizeof(ctx->recv_buffer) - ctx->recv_len);

    if (nread <= 0) {
        // 处理断开连接
        client_close(ctx);
        return;
    }

    ctx->recv_len += nread;

    // 处理完整的数据包
    while (ctx->recv_len > 0) {
        // 检查是否有完整的包头
        if (ctx->recv_len < 4) break;  // 还不够包头长度

        uint32_t pkg_len = *(uint32_t*)ctx->recv_buffer;

        // 检查包长度是否合理
        if (pkg_len > sizeof(ctx->recv_buffer)) {
            printf("包长度过大: %u\n", pkg_len);
            client_close(ctx);
            return;
        }

        // 检查是否收到完整包
        if (ctx->recv_len < pkg_len) {
            break;  // 包不完整，等待更多数据
        }

        // 处理完整包
        char response[4096];
        int response_len = 0;
        process_request(ctx->recv_buffer, pkg_len, response, &response_len);

        // 发送响应
        if (response_len > 0) {
            anetWrite(fd, response, response_len);
        }

        // 从缓冲区移除已处理的数据
        size_t remaining = ctx->recv_len - pkg_len;
        if (remaining > 0) {
            memmove(ctx->recv_buffer, ctx->recv_buffer + pkg_len, remaining);
        }
        ctx->recv_len = remaining;
    }
}

void accept_handler(aeEventLoop* el, int fd, void* privdata, int mask) {
    char ip[64];
    int port;
    int client_fd = anetTcpAccept(NULL, fd, ip, &port);

    if (client_fd == ANET_ERR) return;

    printf("新连接: %s:%d\n", ip, port);

    // 创建客户端上下文
    client_context_t* ctx = zmalloc(sizeof(client_context_t));
    ctx->fd = client_fd;
    ctx->recv_len = 0;
    ctx->parsing = 0;

    anetNonBlock(NULL, client_fd);
    anetTcpNoDelay(NULL, client_fd);

    // 注册事件，传递上下文
    if (aeCreateFileEvent(el, client_fd, AE_READABLE, read_handler, ctx) == AE_ERR) {
        zfree(ctx);
        anetCloseSocket(client_fd);
    }
}


int main(int argc, char* argv[]) {
    int port = 6379; // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // 注册协议处理函数
    register_protocol_handler(1, handle_protocol_1);
    register_protocol_handler(2, handle_protocol_2);

    // 创建事件循环
    aeEventLoop* el = aeCreateEventLoop();
    if (!el) {
        printf("创建事件循环失败\n");
        return 1;
    }

    // 创建TCP服务器
    char err[ANET_ERR_LEN];
    int server_fd = anetTcpServer(err, port, NULL);
    if (server_fd == ANET_ERR) {
        printf("创建服务器失败: %s\n", err);
        return 1;
    }

    // 设置非阻塞模式
    anetNonBlock(err, server_fd);

    // 注册接受连接事件
    if (aeCreateFileEvent(el, server_fd, AE_READABLE, accept_handler, NULL) == AE_ERR) {
        printf("注册接受事件失败\n");
        return 1;
    }

    printf("服务器启动，监听端口 %d\n", port);
    aeMain(el);
    aeDeleteEventLoop(el);

    return 0;
}
