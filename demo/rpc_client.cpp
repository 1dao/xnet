#include "ae.h"
#include "xchannel.h"
#include "xpack.h"
#include "xcoroutine.h"
#include "xrpc.h"
#include "xrpc_template.h"  // 包含模板实现
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// 辅助函数
std::string xpack_buff_to_string(const XPackBuff& buff) {
    if (buff.len <= 0 || !buff.get()) return "";
    return std::string(buff.get(), buff.len);
}

XPackBuff string_to_xpack_buff(const std::string& str) {
    return XPackBuff(str.c_str(), static_cast<int>(str.length()));
}

// 客户端协议处理函数
int client_packet_handler(xChannel* channel, char* buf, int len) {
    return xrpc_resp_blp4(channel) ? len : -1;
}

// 客户端连接关闭处理函数
int client_close_handler(xChannel* channel, char* buf, int len) {
    std::cout << "Connection to server closed" << std::endl;
    return 0;
}

// 综合测试协程
void* comprehensive_test_coroutine(void* arg) {
    return new SimpleTask([arg]() -> SimpleTask {
        std::cout << "=== Comprehensive Test Coroutine Started ===" << std::endl;
		xChannel* g_client_channel = static_cast<xChannel*>(arg);
        if (!g_client_channel) {
            std::cout << "No connection to server" << std::endl;
            co_return;
        }

        // 测试1: 基本运算
        std::cout << "\n--- Testing Basic Arithmetic ---" << std::endl;
        for (int i = 1; i <= 3; i++) {
            XPackBuff result = co_await xrpc_pcall(g_client_channel, 1, i * 5, i * 3);
            if (result.success()) {
                auto unpacked = xpack_unpack(result.get(), result.len);
                if (unpacked.size() >= 2) {
                    int sum = xpack_variant_data<int>(unpacked[0]);
                    std::string status = xpack_buff_to_string(xpack_variant_data<XPackBuff>(unpacked[1]));
                    std::cout << "Test " << i << ": " << (i * 5) << " + " << (i * 3)
                        << " = " << sum << " (" << status << ")" << std::endl;
                }
            }
        }

        // 测试2: 字符串处理
        std::cout << "\n--- Testing String Processing ---" << std::endl;
        std::vector<std::string> test_strings = { "test1", "test2", "test3" };
        for (const auto& str : test_strings) {
            XPackBuff str_buff = string_to_xpack_buff(str);
            XPackBuff result = co_await xrpc_pcall(g_client_channel, 2, str_buff);
            if (result.success()) {
                auto unpacked = xpack_unpack(result.get(), result.len);
                if (unpacked.size() >= 2) {
                    std::string processed = xpack_buff_to_string(xpack_variant_data<XPackBuff>(unpacked[0]));
                    int code = xpack_variant_data<int>(unpacked[1]);
                    std::cout << "String test: '" << str << "' -> '" << processed
                        << "' (code: " << code << ")" << std::endl;
                }
            }
        }

        // 测试3: 错误处理
        std::cout << "\n--- Testing Error Handling ---" << std::endl;

        // 测试无效协议
        XPackBuff error_result = co_await xrpc_pcall(g_client_channel, 999, 1, 2);
        if (!error_result.success()) {
            std::cout << "Error test passed: Got expected error code " << error_result.error_code() << std::endl;
        }

        std::cout << "\n=== Comprehensive Test Coroutine Finished ===" << std::endl;
        co_return;
        }());
}

// 客户端主函数
void client_main() {
    aeEventLoop* el = aeCreateEventLoop();
    if (!el) {
        std::cerr << "Failed to create event loop" << std::endl;
        return;
    }

    if (!coroutine_init()) {
        std::cerr << "Failed to initialize coroutine manager" << std::endl;
        return;
    }

    std::cout << "Connecting to RPC server..." << std::endl;

    xChannel* channel = xchannel_conn((char*)"127.0.0.1", 8888, client_packet_handler, client_close_handler, nullptr);
    if (!channel) {
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }

    std::cout << "Connected to RPC server successfully" << std::endl;

    int add_coro_id = coroutine_run(comprehensive_test_coroutine, channel);
    //int str_coro_id = coroutine_run(string_coroutine, nullptr);

    std::cout << "Started coroutines: Add=" << add_coro_id << ", String=" << "2222" << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    while (coroutine_get_active_count()>0 || std::chrono::steady_clock::now() - start_time < std::chrono::seconds(10)) {
        aeFramePoll(el);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Client finished" << std::endl;
}

int main() {
    client_main();
    return 0;
}