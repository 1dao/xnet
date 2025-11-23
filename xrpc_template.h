#ifndef __XRPC_TEMPLATE_H__
#define __XRPC_TEMPLATE_H__

#include "xrpc.h"
#include "ae.h"
#include "xcoroutine.h"
#include <iostream>

template<typename... Args>
xrpc_awaiter xrpc_pcall(xChannel* channel, uint16_t protocol, Args&&... args) {
    int co_id = coroutine_self_id();
    if (co_id == -1) {
        // 如果当前不在协程上下文，直接返回一个错误 awaiter
        xrpc_awaiter awaiter;
        awaiter.set_error(XRPC_NOT_IN_COROUTINE); // 设置错误码
        std::cout << "xrpc_pcall: Not in coroutine context" << std::endl;
        return awaiter;
    }

    // 创建 awaiter
    auto awaiter = std::make_unique<xrpc_awaiter>();
    awaiter->set_pkg_id(co_id);

    // 构造并发送包
    uint16_t is_req = 1;
    uint32_t pkg_id = co_id; // htonl(co_id);
    XPackBuff packed = xpack_pack(true, protocol, pkg_id, is_req, std::forward<Args>(args)...);

    if (xchannel_send(channel, packed.get(), packed.len) == packed.len) {
        // 注册 RPC 等待
        // RpcResponseManager::instance().register_rpc(co_id, std::move(awaiter));

        aeEventLoop* el = aeGetCurEventLoop();
        if (el) {
            el->nrpc += 1;
        }

        std::cout << "xrpc_pcall: Sent RPC request, pkg_id: " << co_id
                  << ", protocol: " << protocol << std::endl;

        // 返回一个新的 awaiter
        xrpc_awaiter res;
        res.set_pkg_id(co_id);
        return res;
    } else {
        // 如果发送失败，返回一个错误 awaiter
        xrpc_awaiter awaiter;
        awaiter.set_error(XRPC_SEND_FAILED); // 设置发送失败错误码
        std::cout << "xrpc_pcall: Send failed" << std::endl;
        return awaiter;
    }
}

#endif // __XRPC_TEMPLATE_H__