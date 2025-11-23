#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "zmalloc.h"
#include "ae.h"
#include "anet.h"
#include "xchannel.h"
#include "xlog.h"
#include <signal.h>
#define XLOG_TAG "svr"

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
    xlog_info("处理协议1: param1=%d, param2=%.*s\n", param1, param2_len, param2);
    *response_len = sprintf(response, "协议1处理结果: %d", param1 * 2);
    return 0; // 成功
}

int handle_protocol_2(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    xlog_info("处理协议2: param1=%d, param2长度=%d\n", param1, param2_len);
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

    if (pkg_len > (uint32_t)len) {
        xlog_warn("包不全等待继续接受: %d vs %d\n", pkg_len, len);
        return 0;
    }

    if (is_request != 1) {
        xlog_err("不是请求包\n");
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
        xlog_err("未找到协议%d的处理函数\n", protocol);
        return pkg_len +sizeof(int);
    }

    // 构建响应包
    if (need_return) {
        xlog_info("处理完成，长度:% d, 协议 : % d, 包ID : % d\n", pkg_len, protocol, pkg_id);
    
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
		xchannel_rawsend(s, response, resp_pkg_len);
    }
    return pkg_len;
}

int on_close(struct xChannel* s, char* data, int len) {
    xlog_info("连接关闭\n");
    return 0;
}

// 信号处理函数
void signal_handler(int sig) {
    xlog_warn("收到信号 %d，正在关闭应用...", sig);
    xlog_safe_close();
    exit(0);
}

void my_log_hook(int level, const char* tag, const char* message, size_t len, void* userdata) {
    printf("[HOOK] Level:%d Tag:%s Message:%.*s", level, tag, (int)len, message);
}

// 配置日志系统
void setup_logging(void) {
    // 设置日志级别 - 在生产环境中可以设置为 XLOG_WARN 或 XLOG_ERROR
    xlog_set_level(XLOG_DEBUG);

    // 启用文件日志
    xlog_set_file_path("./logs");  // 设置日志文件路径
    xlog_set_file_enable(1);               // 启用文件日志

    // 启用控制台颜色（如果支持）
    xlog_set_show_color(1);
    xlog_set_show_timestamp(1);

    // 4. 启用线程名显示
    xlog_set_show_thread_name(1);
    // 5. 设置主线程名称
    xlog_set_thread_name("MainThread");

    // 6. 注册日志钩子
    // xlog_set_hook(my_log_hook, NULL);

    xlog_warn("日志系统初始化完成\n");
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化日志系统
    setup_logging();

    int port = 6379; // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    register_protocol_handler(1, handle_protocol_1);
    register_protocol_handler(2, handle_protocol_2);

    aeEventLoop* el = aeCreateEventLoop();
    if (!el) {
        xlog_err("创建事件循环失败");
        return 1;
    }

    int res = xchannel_listen(port, NULL, on_packet, on_close, NULL);
    if (res == ANET_ERR) {
        xlog_err("创建服务器失败: %d", res);
        return 1;
    }
    xlog_info("服务器启动，监听端口 %d", port);
    
    aeMain(el);
    while (1) {
        aeFramePoll(el);
        sleep(50);
    }
    aeDeleteEventLoop(el);

    return 0;
}
