#ifndef __XRPC_TEMPLATE_H__
#define __XRPC_TEMPLATE_H__

#include "xrpc.h"
#include "ae.h"
#include "xcoroutine.h"
#include <iostream>

template<typename... Args>
xrpc_awaiter xrpc_pcall_blp4(xChannel* channel, uint16_t protocol, const Args&... args) {
    int co_id = coroutine_self_id();
    if (co_id == -1) {
        xrpc_awaiter awaiter;
        awaiter.set_error(XRPC_NOT_IN_COROUTINE);
        std::cout << "xrpc_pcall_blp4: Not in coroutine context" << std::endl;
        return awaiter;
    }

    // 创建 awaiter 对象
    auto awaiter = std::make_unique<xrpc_awaiter>();
    awaiter->set_pkg_id(co_id);

    // 打包和发送请求
    uint16_t is_req = 1;
    uint32_t pkg_id = co_id; // htonl(co_id);
    XPackBuff packed = xpack_pack(true, protocol, pkg_id, is_req, args...);

    if (xchannel_send(channel, packed.get(), packed.len) == packed.len) {
        // 注册 RPC 等待 - 移动 awaiter
        RpcResponseManager::instance().register_rpc(co_id, std::move(awaiter));

        aeEventLoop* el = aeGetCurEventLoop();
        if (el) {
            el->nrpc += 1;
        }

        std::cout << "xrpc_pcall_blp4: Sent RPC request, pkg_id: " << co_id
            << ", protocol: " << protocol << std::endl;

        // 创建一个新的 awaiter 返回给协程
        xrpc_awaiter result_awaiter;
        result_awaiter.set_pkg_id(co_id);
        return result_awaiter;
    }
    else {
        xrpc_awaiter awaiter;
        awaiter.set_error(XRPC_SEND_FAILED);
        std::cout << "xrpc_pcall_blp4: Send failed" << std::endl;
        return awaiter;
    }
}

#endif // __XRPC_TEMPLATE_H__