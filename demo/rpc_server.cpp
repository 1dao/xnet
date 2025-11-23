#include "ae.h"
#include "xchannel.h"
#include "xpack.h"
#include "xcoroutine.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// 辅助函数：从 XPackBuff 提取字符串
std::string xpack_buff_to_string(const XPackBuff& buff) {
    if (buff.len <= 0 || !buff.get()) return "";
    return std::string(buff.get(), buff.len);
}

// 辅助函数：创建字符串的 XPackBuff
XPackBuff string_to_xpack_buff(const std::string& str) {
    return XPackBuff(str.c_str(), static_cast<int>(str.length()));
}

// 服务器端协议处理函数
int server_packet_handler(xChannel* channel, char* buf, int len) {
    std::cout << "Server received packet, length: " << len << std::endl;

    if (len < 12) {
        std::cout << "Invalid packet length" << std::endl;
        return -1;
    }

    int i = 0;
    std::vector<VariantType> unpacked = xpack_unpack((const char*)buf, (size_t)len);
    auto protocol = xpack_variant_data<uint16_t>(unpacked[i++]);
    auto pkg_id = xpack_variant_data<uint32_t>(unpacked[i++]);
    auto is_rpc = xpack_variant_data<uint16_t>(unpacked[i++]);
    auto arg1 = xpack_variant_data<int>(unpacked[i++]);
    auto arg2 = xpack_variant_data<int>(unpacked[i++]);
    std::cout << "Protocol: " << protocol << ", PkgID: " << pkg_id << ", IsReq: " << is_rpc << std::endl;

    if (is_rpc == 1) {
        if (protocol == 1) {
            // 协议1：加法运算
            if (unpacked.size() >= 2) {
                int a = xpack_variant_data<int>(unpacked[4]);
                int b = 0;// xpack_variant_data<int>(unpacked[5]);
                int result = a + b;

                std::cout << "Processing protocol 1: " << a << " + " << b << " = " << result << std::endl;

                // 打包响应 - 使用 XPackBuff 而不是 std::string
                XPackBuff status_buff = string_to_xpack_buff("success");
                XPackBuff response = xpack_pack(true, result, status_buff);

                // 发送响应
                uint32_t net_pkg_len = htonl(response.len + 8);
                uint16_t net_protocol_resp = htons(protocol);
                uint16_t net_is_req_resp = htons(0);
                uint32_t net_pkg_id_resp = htonl(pkg_id);

                XPackBuff full_response(nullptr, response.len + 12);
                char* resp_buf = const_cast<char*>(full_response.get());

                *(uint32_t*)resp_buf = net_pkg_len;
                *(uint16_t*)(resp_buf + 4) = net_protocol_resp;
                *(uint16_t*)(resp_buf + 6) = net_is_req_resp;
                *(uint32_t*)(resp_buf + 8) = net_pkg_id_resp;
                memcpy(resp_buf + 12, response.get(), response.len);

                xchannel_send(channel, resp_buf, full_response.len);
            }
        } else if (protocol == 2) {
            // 协议2：字符串处理
            if (unpacked.size() >= 1) {
                // 使用 XPackBuff 而不是 std::string
                XPackBuff input_buff = xpack_variant_data<XPackBuff>(unpacked[0]);
                std::string input = xpack_buff_to_string(input_buff);
                std::string result_str = "Processed: " + input;

                std::cout << "Processing protocol 2: " << input << " -> " << result_str << std::endl;

                // 打包响应
                XPackBuff result_buff = string_to_xpack_buff(result_str);
                XPackBuff response = xpack_pack(true, result_buff, 200);

                // 发送响应
                uint32_t net_pkg_len = htonl(response.len + 8);
                uint16_t net_protocol_resp = htons(protocol);
                uint16_t net_is_req_resp = htons(0);
                uint32_t net_pkg_id_resp = htonl(pkg_id);

                XPackBuff full_response(nullptr, response.len + 12);
                char* resp_buf = const_cast<char*>(full_response.get());

                *(uint32_t*)resp_buf = net_pkg_len;
                *(uint16_t*)(resp_buf + 4) = net_protocol_resp;
                *(uint16_t*)(resp_buf + 6) = net_is_req_resp;
                *(uint32_t*)(resp_buf + 8) = net_pkg_id_resp;
                memcpy(resp_buf + 12, response.get(), response.len);

                xchannel_send(channel, resp_buf, full_response.len);
            }
        }
    }

    return len;
}

// 服务器端连接关闭处理函数
int server_close_handler(xChannel* channel, char* buf, int len) {
    std::cout << "Client disconnected, fd: " << channel->fd << std::endl;
    return 0;
}

// 服务器主循环
void server_main() {
    aeEventLoop* el = aeCreateEventLoop();
    if (!el) {
        std::cerr << "Failed to create event loop" << std::endl;
        return;
    }

    if (!coroutine_init()) {
        std::cerr << "Failed to initialize coroutine manager" << std::endl;
        return;
    }

    std::cout << "Starting RPC server on port 8888..." << std::endl;

    if (xchannel_listen(8888, (char*)"127.0.0.1", server_packet_handler, server_close_handler, nullptr) == AE_ERR) {
        std::cerr << "Failed to start server" << std::endl;
        return;
    }

    std::cout << "RPC server started successfully" << std::endl;
    aeMain(el);
}

int main() {
    server_main();
    return 0;
}