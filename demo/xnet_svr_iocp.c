#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "zmalloc.h"
#include "ae.h"
#include "anet.h"
#include "xchannel.h"

typedef int (*ProtocolHandler)(int param1, const char* param2, int param2_len, char* response, int* response_len);

typedef struct {
    uint16_t protocol;
    ProtocolHandler handler;
} ProtocolMapping;

static ProtocolMapping protocol_handlers[256];
static int handler_count = 0;

void register_protocol_handler(uint16_t protocol, ProtocolHandler handler) {
    if (handler_count < 256) {
        protocol_handlers[handler_count].protocol = protocol;
        protocol_handlers[handler_count].handler = handler;
        handler_count++;
    }
}

ProtocolHandler find_protocol_handler(uint16_t protocol) {
    for (int i = 0; i < handler_count; i++) {
        if (protocol_handlers[i].protocol == protocol) {
            return protocol_handlers[i].handler;
        }
    }
    return NULL;
}

int handle_protocol_1(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    printf("处理协议1: param1=%d, param2=%.*s\n", param1, param2_len, param2);
    *response_len = sprintf(response, "协议1处理结果: %d", param1 * 2);
    return 0; // 成功
}

int handle_protocol_2(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    printf("处理协议2: param1=%d, param2长度=%d\n", param1, param2_len);
    *response_len = sprintf(response, "协议2处理结果: %d字节数据", param2_len);
    return 0; // 成功
}

void sleep(unsigned int milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#elif __linux__
    usleep(milliseconds * 1000);  // usleep 使用微秒
#endif
}

int on_packet(struct xChannel* s, char* buf, int len) {
    if (len < 12) { // 最小请求包长度：4+2+1+1+4=12字节
        return 0;
    }

    uint32_t pkg_len = *(const uint32_t*)buf;
    uint16_t protocol = *(const uint16_t*)(buf + 4);
    uint8_t need_return = *(const uint8_t*)(buf + 6);
    uint8_t is_request = *(const uint8_t*)(buf + 7);
    uint32_t pkg_id = *(const uint32_t*)(buf + 8);

    if (pkg_len > len) {
        printf("包不全等待继续接受: %d vs %d\n", pkg_len, len);
        return 0;
    }

    if (is_request != 1) {
        printf("不是请求包\n");
        return pkg_len + sizeof(int);
    }

    // 解析参数
    int param1 = 0;
    const char* param2 = NULL;
    int param2_len = 0;

    if (len > 12) {
        param1 = *(const int*)(buf + 12);
        if (len > 16) {
            param2 = buf + 16;
            param2_len = len - 16;
        }
    }

    // 查找并调用对应的协议处理函数
    ProtocolHandler handler = find_protocol_handler(protocol);
    char handler_response[1024] = { 0 };
    int handler_response_len = 0;
    int ret = -1;

    if (handler) {
        ret = handler(param1, param2, param2_len, handler_response, &handler_response_len);
    } else {
        printf("未找到协议%d的处理函数\n", protocol);
        return pkg_len +sizeof(int);
    }

    // 构建响应包
    if (need_return) {
        printf("处理完成，长度:% d, 协议 : % d, 包ID : % d\n", pkg_len, protocol, pkg_id);

        // 响应包头部长度：4+2+1+1+4=12字节
        char response[1024];
        uint32_t resp_pkg_len = handler_response_len +12;
        uint8_t return_flag = 0; // 不返回
        uint8_t is_response = 0; // 返回包

        memcpy(response, &resp_pkg_len, 4);
        memcpy(response + 4, &protocol, 2);
        memcpy(response + 6, &return_flag, 1);
        memcpy(response + 7, &is_response, 1);
        memcpy(response + 8, &pkg_id, 4);
        memcpy(response + 12, handler_response, handler_response_len);
		xchannel_send(s, response, resp_pkg_len);
    }
    return pkg_len;
}

int on_close(struct xChannel* s, char* data, int len) {
    printf("连接关闭\n");
    return 0;
}

int main(int argc, char* argv[]) {
    int port = 6379; // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    register_protocol_handler(1, handle_protocol_1);
    register_protocol_handler(2, handle_protocol_2);

    aeEventLoop* el = aeCreateEventLoop();
    if (!el) {
        printf("创建事件循环失败\n");
        return 1;
    }

    int server_fd = xchannel_listen(port, NULL, on_packet, on_close, NULL);
    if (server_fd == ANET_ERR) {
        printf("创建服务器失败: %d\n", server_fd);
        return 1;
    }

    printf("服务器启动，监听端口 %d\n", port);
    aeMain(el);
    while (1) {
        aeProcessEvents(el, AE_ALL_EVENTS | AE_DONT_WAIT);
        sleep(50);
    }
    aeDeleteEventLoop(el);

    return 0;
}
