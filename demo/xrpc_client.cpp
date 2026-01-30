#include "ae.h"
#include "xchannel.h"
#include "xpack.h"
#include "xcoroutine.h"
#include "xrpc.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "xlog.h"

// 辅助函数
std::string xpack_cast_string(const XPackBuff& buff) {
    if (buff.len <= 0 || !buff.get()) return "";
    return std::string(buff.get(), buff.len);
}

XPackBuff xpack_cast_buff(const std::string& str) {
    return XPackBuff(str.c_str(), static_cast<int>(str.length()));
}

// 客户端连接关闭处理函数
int client_close_handler(xChannel* channel, char* buf, int len) {
    std::cout << "Connection to server closed" << std::endl;
    return 0;
}

//=============================================================================
// 测试用例 1: 基本 RPC 调用测试
//=============================================================================
xCoroTask test_basic_rpc(void* arg) {
    xChannel* channel = static_cast<xChannel*>(arg);
    xlog_info("=== Test 1: Basic RPC Call ===");

    // 调用协议1
    auto result = co_await xrpc_pcall(channel, 1, 100, 200, XPackBuff("hello"));

    // 检查结果
    int retcode = xrpc_retcode(result);
    if (retcode != 0) {
        xlog_err("RPC failed, retcode: %d", retcode);
        co_return;
    }

    // 解析返回数据（从 index 1 开始）
    // result[0] = retcode
    // result[1] = 第一个返回值
    // result[2] = 第二个返回值
    // ...
    xlog_info("RPC success!");
    xlog_info("  retcode: %d", retcode);
    xlog_info("  data[0]: %d", xpack_cast<int>(result[1]));
    xlog_info("  data[1]: %d", xpack_cast<int>(result[2]));
    xlog_info("  data[2]: %d", xpack_cast<int>(result[3]));
    xlog_info("  data[3]: %s", xpack_cast_string(xpack_cast<XPackBuff>(result[4])).c_str());

    xlog_info("=== Test 1 Completed ===\n");
    co_return;
}

//=============================================================================
// 测试用例 2: 多次 RPC 调用测试
//=============================================================================
xCoroTask test_multiple_rpc(void* arg) {
    xChannel* channel = static_cast<xChannel*>(arg);
    xlog_info("=== Test 2: Multiple RPC Calls ===");

    for (int i = 1; i <= 3; i++) {
        xlog_info("--- Call %d ---", i);

        auto result = co_await xrpc_pcall(channel, 1, i * 10, i * 20, XPackBuff("test"));

        if (!xrpc_ok(result)) {
            xlog_err("Call %d failed, retcode: %d", i, xrpc_retcode(result));
            continue;
        }

        xlog_info("Call %d success: v1=%d, v2=%d",
                  i,
                  xpack_cast<int>(result[1]),
                  xpack_cast<int>(result[2]));
    }

    xlog_info("=== Test 2 Completed ===\n");
    co_return;
}

//=============================================================================
// 测试用例 3: 错误处理测试
//=============================================================================
xCoroTask test_error_handling(void* arg) {
    xChannel* channel = static_cast<xChannel*>(arg);
    xlog_info("=== Test 3: Error Handling ===");

    // 调用不存在的协议
    xlog_info("--- Testing invalid protocol ---");
    auto result = co_await xrpc_pcall(channel, 999, 1, 2);

    int retcode = xrpc_retcode(result);
    if (retcode != 0) {
        xlog_info("Expected error received, retcode: %d", retcode);
    } else {
        xlog_err("Error test failed: expected error but got success");
    }

    xlog_info("=== Test 3 Completed ===\n");
    co_return;
}

//=============================================================================
// 测试用例 4: 字符串处理测试
//=============================================================================
xCoroTask test_string_processing(void* arg) {
    xChannel* channel = static_cast<xChannel*>(arg);
    xlog_info("=== Test 4: String Processing ===");

    std::vector<std::string> test_strings = {"hello", "world", "test123"};

    for (const auto& str : test_strings) {
        auto result = co_await xrpc_pcall(channel, 2, 0, 0, XPackBuff(str.c_str()));

        if (!xrpc_ok(result)) {
            xlog_err("String test failed for '%s', retcode: %d",
                     str.c_str(), xrpc_retcode(result));
            continue;
        }

        xlog_info("String '%s' processed successfully", str.c_str());
        if (result.size() > 4) {
            xlog_info("  Response: %s",
                      xpack_cast_string(xpack_cast<XPackBuff>(result[4])).c_str());
        }
    }

    xlog_info("=== Test 4 Completed ===\n");
    co_return;
}

//=============================================================================
// 测试用例 5: 综合测试
//=============================================================================
xCoroTask test_comprehensive(void* arg) {
    xChannel* channel = static_cast<xChannel*>(arg);
    xlog_info("=== Test 5: Comprehensive Test ===");

    // 第一次 RPC 调用
    xlog_info("--- First RPC call ---");
    auto result1 = co_await xrpc_pcall(channel, 1, 333, 7777, XPackBuff("first"));

    if (!xrpc_ok(result1)) {
        xlog_err("First RPC failed, retcode: %d", xrpc_retcode(result1));
        co_return;
    }
    xlog_info("First RPC success, data[0]: %d", xpack_cast<int>(result1[1]));

    // 第二次 RPC 调用（测试同一协程内多次调用）
    xlog_info("--- Second RPC call ---");
    auto result2 = co_await xrpc_pcall(channel, 1, 666, 888, XPackBuff("second"));

    if (!xrpc_ok(result2)) {
        xlog_err("Second RPC failed, retcode: %d", xrpc_retcode(result2));
        co_return;
    }
    xlog_info("Second RPC success, data[0]: %d", xpack_cast<int>(result2[1]));

    // 第三次调用不同协议
    xlog_info("--- Third RPC call (different protocol) ---");
    auto result3 = co_await xrpc_pcall(channel, 2, 111, 222, XPackBuff("third"));

    if (!xrpc_ok(result3)) {
        xlog_err("Third RPC failed, retcode: %d", xrpc_retcode(result3));
        co_return;
    }
    xlog_info("Third RPC success");

    xlog_info("=== Test 5 Completed ===\n");
    co_return;
}

//=============================================================================
// 主函数
//=============================================================================
void client_main() {
    aeEventLoop* el = aeCreateEventLoop(100);
    if (!el) {
        std::cerr << "Failed to create event loop" << std::endl;
        return;
    }

    if (!coroutine_init()) {
        std::cerr << "Failed to initialize coroutine manager" << std::endl;
        return;
    }

    std::cout << "Connecting to RPC server..." << std::endl;

    xChannel* channel = xchannel_conn((char*)"127.0.0.1", 8888, NULL, client_close_handler, nullptr);
    if (!channel) {
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }

    std::cout << "Connected to RPC server successfully\n" << std::endl;

    // 运行测试用例
    coroutine_run(test_basic_rpc, channel);
    coroutine_run(test_multiple_rpc, channel);
    coroutine_run(test_error_handling, channel);
    coroutine_run(test_string_processing, channel);
    coroutine_run(test_comprehensive, channel);

    // 事件循环
    while (true) {
        aeProcessEvents(el, AE_ALL_EVENTS | AE_DONT_WAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    coroutine_uninit();
    std::cout << "Client finished" << std::endl;
}

int main() {
    client_main();
    return 0;
}
