#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>

#include "ae.h"
#include "anet.h"
#include "xchannel.h"
#include "zmalloc.h"
#include "xcoroutine.h"
#include "xlog.h"

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

void xnet_sleep(unsigned int milliseconds) {  // 重命名避免与标准库冲突
#ifdef _WIN32
    Sleep(milliseconds);
#elif __linux__
    usleep(milliseconds * 1000);  // usleep 使用微秒
#endif
}

typedef struct {
    uint32_t pkg_len;       // 包长度4字节
    uint16_t protocol;      // 协议2字节
    uint8_t need_return;    // 是否返回1字节
    uint8_t is_request;     // 是请求还是返回1字节(1:请求,0:返回)
    uint32_t pkg_id;        // 协议包ID4字节
    int param1;             // 参数1整型
    // 参数2二进制数据紧随其后
} ProtocolPacket;

// 构建请求包
char* build_request_packet(uint16_t protocol, uint8_t need_return,
    uint32_t pkg_id, int param1,
    const char* param2, int param2_len,
    int* packet_len) {
    // 计算包总长度：头部长度 + 参数1长度 + 参数2长度
    int header_len = sizeof(ProtocolPacket) - sizeof(int); // 减去param1的大小，因为已经包含在结构中
    *packet_len = header_len + sizeof(int) + param2_len;

    // 分配内存
    char* packet = (char*)zmalloc(*packet_len);
    if (!packet) return NULL;

    // 填充头部信息
    ProtocolPacket* pkg = (ProtocolPacket*)packet;
    pkg->pkg_len = *packet_len;
    pkg->protocol = protocol;
    pkg->need_return = need_return;
    pkg->is_request = 1;  // 请求包
    pkg->pkg_id = pkg_id;
    pkg->param1 = param1;

    // 填充参数2
    if (param2 && param2_len > 0) {
        memcpy(packet + sizeof(ProtocolPacket), param2, param2_len);
    }

    return packet;
}

// 解析响应包
int parse_response_packet(const char* response, int response_len,
    ProtocolPacket* pkg, char** param2, int* param2_len) {
    if (response_len < sizeof(ProtocolPacket)) {
        printf("响应包长度不足\n");
        return -1;
    }

    // 复制头部信息
    memcpy(pkg, response, sizeof(ProtocolPacket));

    // 验证包长度
    if (pkg->pkg_len != response_len) {
        printf("响应包包长度不匹配: %d vs %d\n", pkg->pkg_len, response_len);
        return -1;
    }

    // 验证是否为返回包
    if (pkg->is_request != 0) {
        printf("不是返回包\n");
        return -1;
    }

    // 提取参数2
    *param2_len = response_len - sizeof(ProtocolPacket);
    if (*param2_len > 0) {
        *param2 = (char*)zmalloc(*param2_len + 1);
        memcpy(*param2, response + sizeof(ProtocolPacket), *param2_len);
        (*param2)[*param2_len] = '\0'; // 确保字符串结束
    }
    else {
        *param2 = NULL;
    }

    return 0;
}

int send_msg(xChannel* self, uint16_t protocol, bool is_rpc, const char* data, int len) {
    // 准备发送的数据
    uint8_t need_return = is_rpc ? 1 : 0;
    static uint32_t pkg_id = 111;
    int param1 = 100;

    std::vector<char> vdata;
    vdata.push_back('a');
    vdata.push_back('c');
    vdata.push_back('b');
    vdata.push_back('d');

    // 构建请求包
    int packet_len;
    char* request_packet = build_request_packet(protocol, need_return,
        pkg_id, param1, vdata.data(), (int)vdata.size(), &packet_len);

    if (!request_packet) {
        printf("构建请求包失败\n");
        return -1;
    }

    // 发送请求包
    printf("发送请求包 - 长度: %d, 协议: %d, 包ID: %d\n", packet_len, protocol, pkg_id);
    pkg_id++;
    int send_len = xchannel_rawsend(self, request_packet, packet_len);
    if (send_len != packet_len) {
        printf("发送数据失败，发送了 %d/%d 字节\n", send_len, packet_len);
        zfree(request_packet);
        return -1;
    }
    zfree(request_packet);
    return need_return;
}

int on_packet(struct xChannel* s, char* buf, int len) {
    // 检查是否有完整的包头
    if (len < 4) return 0;  // 还不够包头长度

    uint32_t pkg_len = *(uint32_t*)buf;
    // 检查是否收到完整包
    if (len < (int)pkg_len) {
        return 0;  // 包不完整，等待更多数据
    }

    ProtocolPacket resp_pkg;
    char* resp_param2;
    int resp_param2_len;
    // (char*)((uint32_t*)buf + 1)
    if (parse_response_packet(buf, pkg_len, &resp_pkg, &resp_param2, &resp_param2_len) == 0) {
        printf("收到响应 - 协议: %d, 包ID: %d, 参数1: %d\n",
            resp_pkg.protocol, resp_pkg.pkg_id, resp_pkg.param1);
        if (resp_param2 && resp_param2_len > 0) {
            printf("响应数据: %.*s\n", resp_param2_len, resp_param2);
            zfree(resp_param2);
        }
    } else {
        printf("解析响应包失败\n");
    }

    printf("客户端协程任务完成\n");
    return pkg_len;
}

int running = 1;
int on_close(struct xChannel* s, char* buf, int len) {
    printf("连接关闭\n");
    running = 0;
    return 0;
}

int main(int argc, char* argv[]) {
    const char* ip = "127.0.0.1";
    int port = 6379;
    char err[ANET_ERR_LEN];

    // 创建TCP连接
    xChannel* net_client = xchannel_conn((char*)ip, port, on_packet, on_close, NULL);
    if (!net_client) {
        printf("连接服务器失败: %s\n", err);
        return 1;
    }

    // 创建事件循环
    aeEventLoop* el = aeCreateEventLoop(100);
    if (!el) {
        printf("创建事件循环失败\n");
        return 1;
    }

    // using coroutine to handle client logic
    printf("连接服务器成功，开始协程客户端...\n");
    const char* st = "这是测试数据";

    // 运行调度器
    while (running) {
        aeProcessEvents(el, AE_ALL_EVENTS | AE_DONT_WAIT);
        xnet_sleep(500);  // 使用重命名后的函数
        if (send_msg(net_client, 1, true, st, (int)strlen(st)) > 0) {
        }
    }
    // using a coroutine to handle client logic finish

    aeDeleteEventLoop(el);
    printf("客户端已关闭\n");

    return 0;
}
