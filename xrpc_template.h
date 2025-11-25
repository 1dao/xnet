#ifndef __XRPC_TEMPLATE_H__
#define __XRPC_TEMPLATE_H__

#include <iostream>

#include "xrpc.h"
#include "ae.h"
#include "xcoroutine.h"
#include "xchannel.inl"

template<typename... Args>
xrpc_awaiter xrpc_pcall(xChannel* s, uint16_t protocol, Args&&... args) {
    int co_id = coroutine_self_id();
    if (co_id == -1) {
        // 如果当前不在协程上下文，直接返回一个错误 awaiter
        xrpc_awaiter awaiter;
        awaiter.set_error(XRPC_NOT_IN_COROUTINE); // 设置错误码
        std::cout << "xrpc_pcall: Not in coroutine context" << std::endl;
        return awaiter;
    }

    uint16_t is_rpc = 1;
    uint32_t pkg_id = co_id;
    XPackBuff packed = xpack_pack(true, std::forward<Args>(args)...);
    char* wpos = s->wpos;
    int remain = s->wlen - (s->wpos - s->wbuf);
    int hlen = _xchannel_header_size(s);
    int plen = (packed.len + sizeof(pkg_id) + sizeof(co_id) + sizeof(is_rpc)+ sizeof(protocol));
    if (remain < hlen + plen) {
        // 如果缓冲区空间不足，直接返回一个错误 awaiter
        xrpc_awaiter awaiter;
        awaiter.set_error(XRPC_SEND_FAILED); // 设置错误码
        std::cout << "xrpc_pcall: Buffer overflow" << std::endl;
        return awaiter;
    }
    _xchannel_write_header(s, plen);

    // 创建 awaiter
    auto awaiter = std::make_unique<xrpc_awaiter>();
    awaiter->set_pkg_id(co_id);

    // 写RPC头
    *(uint16_t*)s->wpos = htons(is_rpc);
    s->wpos += sizeof(is_rpc);
    *(uint32_t*)s->wpos = htonl(pkg_id);
    s->wpos += sizeof(pkg_id);
    *(int*)s->wpos = htonl(co_id);
    s->wpos += sizeof(co_id);
    *(uint16_t*)s->wpos = htons(protocol);
    s->wpos += sizeof(protocol);

    if (xchannel_rawsend(s, packed.get(), packed.len) == packed.len) {
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
        s->wpos = wpos;
        std::cout << "xrpc_pcall: Send failed" << std::endl;
        return awaiter;
    }
}

#endif // __XRPC_TEMPLATE_H__
