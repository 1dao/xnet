#include "ae.h"
#include "xchannel.h"
#include "xpack.h"
#include "xcoroutine.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "xhandle.h"
#include "xlog.h"

// 服务器端连接关闭处理函数
int sock_on_closed(xChannel* channel, char* buf, int len) {
    xlog_info("Client disconnected, fd: %d", channel->fd);
    return 0;
}

// 协议1处理函数：基本运算
XPackBuff on_pt1(xChannel* s, std::vector<VariantType>& args) {
    if (args.size() < 3) {
        xlog_err("Protocol 1: invalid args count: %zu", args.size());
        throw std::runtime_error("Invalid arguments");
    }

    auto arg1 = xpack_cast<int>(args[0]);
    auto arg2 = xpack_cast<int>(args[1]);
    auto arg3 = xpack_cast<XPackBuff>(args[2]);

    xlog_info("Protocol 1: arg1=%d, arg2=%d, arg3=%s",
              arg1, arg2, arg3.get() ? arg3.get() : "null");

    // 返回数据
    int sum = arg1 + arg2;
    int diff = arg1 - arg2;
    int product = arg1 * arg2;

    return xpack_pack(true, sum, diff, product, XPackBuff("pt1 success"));
}

// 协议2处理函数：字符串处理
XPackBuff on_pt2(xChannel* s, std::vector<VariantType>& args) {
    if (args.size() < 3) {
        xlog_err("Protocol 2: invalid args count: %zu", args.size());
        throw std::runtime_error("Invalid arguments");
    }

    auto arg1 = xpack_cast<int>(args[0]);
    auto arg2 = xpack_cast<int>(args[1]);
    auto arg3 = xpack_cast<XPackBuff>(args[2]);

    std::string input_str = arg3.get() ? std::string(arg3.get(), arg3.len) : "";
    xlog_info("Protocol 2: processing string '%s'", input_str.c_str());

    // 处理字符串（转大写）
    std::string result_str = "Processed: " + input_str;

    return xpack_pack(true, 200, 0, 0, XPackBuff(result_str.c_str()));
}

// 协议3处理函数：测试异常
XPackBuff on_pt3(xChannel* s, std::vector<VariantType>& args) {
    xlog_info("Protocol 3: throwing exception for test");
    throw std::runtime_error("Test exception from protocol 3");
}

// 注册协议处理函数
void pack_handles_reg() {
    xhandle_reg_rpc(1, on_pt1);
    xhandle_reg_rpc(2, on_pt2);
    xhandle_reg_rpc(3, on_pt3);
    xlog_info("Registered %d RPC handlers", 3);
}

// 服务器主循环
void server_main() {
    aeEventLoop* el = aeCreateEventLoop(100);
    if (!el) {
        std::cerr << "Failed to create event loop" << std::endl;
        return;
    }

    if (!coroutine_init()) {
        std::cerr << "Failed to initialize coroutine manager" << std::endl;
        return;
    }

    xlog_info("Starting RPC server on port 8888...");

    if (xchannel_listen(8888, (char*)"127.0.0.1", NULL, sock_on_closed, nullptr) == AE_ERR) {
        std::cerr << "Failed to start server" << std::endl;
        return;
    }

    pack_handles_reg();

    xlog_info("RPC server started successfully");
    aeMain(el);

    coroutine_uninit();
}

int main() {
    server_main();
    return 0;
}
