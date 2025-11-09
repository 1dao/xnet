#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <vector>

#include "anet.h"
#include "zmalloc.h"
#include "coroutine.h"

// 平台检测-for sleep
#ifdef _WIN32
#include <windows.h>
#elif __linux__
#include <unistd.h>
#else
#error "Unsupported platform"
#endif

void cross_sleep(unsigned int milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#elif __linux__
    usleep(milliseconds * 1000);  // usleep 使用微秒
#endif
}

// 协议包结构定义
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
    } else {
        *param2 = NULL;
    }

    return 0;
}

int send_msg(int fd, uint16_t protocol, bool is_rpc, const char* data, int len) {
    // 准备发送的数据
    uint8_t need_return = is_rpc?1:0;
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
        pkg_id, param1, vdata.data(), vdata.size(), &packet_len);

    if (!request_packet) {
        printf("构建请求包失败\n");
        return -1;
    }

    // 发送请求包
    printf("发送请求包 - 长度: %d, 协议: %d, 包ID: %d\n", packet_len, protocol, pkg_id);

    int send_len = anetWrite(fd, request_packet, packet_len);
    zfree(request_packet);

    if (send_len != packet_len) {
        printf("发送数据失败，发送了 %d/%d 字节\n", send_len, packet_len);
        return -1;
    }
    return need_return;
}

// 协程化的客户端逻辑
void client_coroutine(int fd) {
    // 使用协程方式接收响应
    char response[4096];
    int recv_len = 0;
    bool packet_complete = false;
    long long start_time = coroutine_current_time();

    while (!packet_complete) {
        // 检查超时
        if (coroutine_current_time() - start_time > 5000) {
            printf("接收响应超时\n");
            return;
        }

        // 尝试读取数据
        int n = anetReadWithTimeout(fd, response + recv_len, sizeof(response) - recv_len, 0);
        if (n > 0) {
            recv_len += n;

            // 检查是否收到完整包头
            if (recv_len >= sizeof(ProtocolPacket)) {
                ProtocolPacket* header = (ProtocolPacket*)response;

                // 验证包长度
                if (header->pkg_len > sizeof(response)) {
                    printf("包长度超出缓冲区大小\n");
                    return;
                }

                // 检查是否收到完整包
                if (recv_len >= header->pkg_len) {
                    packet_complete = true;
                }
            }
        }
        else if (n == 0) {
            printf("连接被服务器关闭\n");
            return;
        } else {
            // 没有数据可读，挂起协程等待数据
            return coroutine_wait_read(fd, 5000 - (coroutine_current_time() - start_time));
        }
    }

    // 处理完整响应包
    ProtocolPacket resp_pkg;
    char* resp_param2;
    int resp_param2_len;

    if (parse_response_packet(response, recv_len, &resp_pkg, &resp_param2, &resp_param2_len) == 0) {
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
}

int main(int argc, char* argv[]) {
    //if (argc < 3) {
    //    printf("使用方法: %s <服务器IP> <端口>\n", argv[0]);
    //    return 1;
    //}

    //char* ip = argv[1];
    //int port = atoi(argv[2]);

    const char* ip = "127.0.0.1";
    int port = 6379;
    char err[ANET_ERR_LEN];

    // 创建TCP连接
    int fd = anetTcpConnect(err, (char*)ip, port);
    if (fd == ANET_ERR) {
        printf("连接服务器失败: %s\n", err);
        return 1;
    }

    // 设置TCP_NODELAY
    if (anetTcpNoDelay(err, fd) != ANET_OK) {
        printf("设置TCP_NODELAY失败: %s\n", err);
        anetCloseSocket(fd);
        return 1;
    }

    //// 准备发送的数据
    //uint16_t protocol = 1;         // 协议号1
    //uint8_t need_return = 1;       // 需要返回
    //uint32_t pkg_id = 12345;       // 包ID
    //int param1 = 100;              // 参数1
    //const char* param2 = "这是测试数据"; // 参数2
    //int param2_len = strlen(param2);

    //// 构建请求包
    //int packet_len;
    //std::vector<char> vdata;
    //vdata.push_back('a');
    //vdata.push_back('c');
    //vdata.push_back('b');
    //vdata.push_back('d');
    //char* request_packet = build_request_packet(protocol, need_return,
    //    pkg_id, param1,
    //    vdata.data(), vdata.size(),
    //    &packet_len);
    //if (!request_packet) {
    //    printf("构建请求包失败\n");
    //    anetCloseSocket(fd);
    //    return 1;
    //}

    //// 发送请求包
    //printf("发送请求包 - 长度: %d, 协议: %d, 包ID: %d\n",
    //    packet_len, protocol, pkg_id);
    //int send_len = anetWrite(fd, request_packet, packet_len);
    //if (send_len != packet_len) {
    //    printf("发送数据失败，发送了 %d/%d 字节\n", send_len, packet_len);
    //    zfree(request_packet);
    //    anetCloseSocket(fd);
    //    return 1;
    //}
    //zfree(request_packet);

    //// 如果需要返回，等待接收响应
    //if (need_return) {
    //    char response[4096];
    //    int recv_len = anetRead(fd, response, sizeof(response) - 1);
    //    if (recv_len <= 0) {
    //        printf("接收响应失败: %d\n", recv_len);
    //        anetCloseSocket(fd);
    //        return 1;
    //    }

    //    // 解析响应
    //    ProtocolPacket resp_pkg;
    //    char* resp_param2;
    //    int resp_param2_len;

    //    if (parse_response_packet(response, recv_len, &resp_pkg, &resp_param2, &resp_param2_len) == 0) {
    //        printf("收到响应 - 协议: %d, 包ID: %d, 参数1: %d\n",
    //            resp_pkg.protocol, resp_pkg.pkg_id, resp_pkg.param1);
    //        if (resp_param2 && resp_param2_len > 0) {
    //            printf("响应数据: %s\n", resp_param2);
    //            zfree(resp_param2);
    //        }
    //    }
    //}

    // using coroutine to handle client logic
    printf("连接服务器成功，开始协程客户端...\n");

    const char* st = "这是测试数据";
    if (send_msg(fd, 1, true, st, strlen(st)) > 0) {
        // 创建客户端协程任务
        coroutine_add_task([fd]() {
            client_coroutine(fd);
        });
    }

    // 运行调度器
    while (true) {
        coroutine_update();
        cross_sleep(5000);
        if (send_msg(fd, 1, true, st, strlen(st)) > 0) {
            // 创建客户端协程任务
            coroutine_add_task([fd]() {
                client_coroutine(fd);
                });
        }
    }
    // using a coroutine to handle client logic finish
    
    // 关闭连接
    anetCloseSocket(fd);
    printf("客户端已关闭\n");

    return 0;
}
