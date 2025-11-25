#include "ae.h"
#include "xchannel.h"
#include "xpack.h"
#include "xcoroutine.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "xhandle.h"


// 服务器端连接关闭处理函数
int server_close_handler(xChannel* channel, char* buf, int len) {
    std::cout << "Client disconnected, fd: " << channel->fd << std::endl;
    return 0;
}

XPackBuff on_pt1(xChannel* s, std::vector<VariantType>& args) {
    auto arg1 = xpack_variant_data<int>(args[0]);
    auto arg2 = xpack_variant_data<int>(args[1]);
    auto arg3 = xpack_variant_data<XPackBuff>(args[2]);
    std::cout << "Protocol: " << 1 << ", arg1: " << arg1 << ", arg2: " << arg2 << ", arg3: " << arg3.get() << std::endl;
    return xpack_pack(true, 555, -111, 666, XPackBuff("success"));
}

XPackBuff on_pt2(xChannel* s, std::vector<VariantType>& args) {
    auto arg1 = xpack_variant_data<int>(args[0]);
    auto arg2 = xpack_variant_data<int>(args[1]);
    auto arg3 = xpack_variant_data<XPackBuff>(args[2]);
    std::cout << "Protocol: " << 2 << ", arg1: " << arg1 << ", arg2: " << arg2 << ", arg3: " << arg3.get() << std::endl;
    return xpack_pack(true, 555, -111, 666, XPackBuff("success"));
}

void pack_handles_reg() {
    xhandle_reg_rpc(1, on_pt1);
    xhandle_reg_rpc(2, on_pt2);
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

    if (xchannel_listen(8888, (char*)"127.0.0.1", NULL, server_close_handler, nullptr) == AE_ERR) {
        std::cerr << "Failed to start server" << std::endl;
        return;
    }
    pack_handles_reg();

    std::cout << "RPC server started successfully" << std::endl;
    aeMain(el);

    coroutine_uninit();
}

int main() {
    server_main();
    return 0;
}
