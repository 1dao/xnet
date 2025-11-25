#ifndef __XRPC_INL_H__
#define __XRPC_INL_H__

#include <iostream>
#include <utility>
#include <type_traits>

#include "ae.h"
#include "xcoroutine.h"
#include "xchannel.inl"

// RpcCall: 一个轻量对象，提供 to_awaiter(pkg_id)
struct RpcCall {
    uint32_t pkg_id;
    std::vector<VariantType> resp;

    RpcCall(uint32_t _pkg_id, std::vector<VariantType>&& res)
        : pkg_id(_pkg_id), resp(std::move(res)) {
    }

    // called by promise.yield_value(...). 返回 awaiter (见下)
    auto awaiter_cvt() const;
};

// 实现 RpcCall::to_awaiter（需要 xAwaiter 可用）
inline auto RpcCall::awaiter_cvt() const {
    // 到这里，packet 应该已经发送（见 rpc_pcall），我们只返回 awaiter
    return xAwaiter(pkg_id);
}

// 直接发包并返回 RpcCall（pkg_id 用协程 id）
template<typename... Args>
RpcCall xrpc_pcall(xChannel* s, uint16_t protocol, Args&&... args) {
    int co_id = coroutine_self_id();
    if (co_id == -1) {
        // 返回一个空的 RpcCall（后续 await_resume 将判错）
        std::vector<VariantType> res;
        res.emplace_back(int(XNET_NOT_IN_COROUTINE));
        return RpcCall{ 0, std::move(res)};
    }

    uint16_t is_rpc = 1;
    uint32_t pkg_id = (uint32_t)co_id;
    XPackBuff packed = xpack_pack(true, std::forward<Args>(args)...);

    char* wpos = s->wpos;
    int remain = (int)s->wlen - (int)(s->wpos - s->wbuf);
    int hlen = (int)_xchannel_header_size(s);
    int plen = (packed.len + sizeof(pkg_id) + sizeof(co_id) + sizeof(is_rpc)+ sizeof(protocol));
    if (remain < hlen + plen) {
        // buffer insufficient -> return dummy call
        std::cout << "rpc_pcall: Buffer overflow" << std::endl;
        std::vector<VariantType> res;
        res.emplace_back(int(XNET_BUFF_LIMIT));
        return RpcCall{0, std::move(res)};
    }
    _xchannel_write_header(s, plen);

    // 写RPC头
    *(uint16_t*)s->wpos = htons(is_rpc);
    s->wpos += sizeof(is_rpc);
    *(uint32_t*)s->wpos = htonl(pkg_id);
    s->wpos += sizeof(pkg_id);
    *(int*)s->wpos = htonl(co_id);
    s->wpos += sizeof(co_id);
    *(uint16_t*)s->wpos = htons(protocol);
    s->wpos += sizeof(protocol);

    int sendlen = xchannel_rawsend(s, packed.get(), packed.len);
    if (sendlen != packed.len) {
        std::cout << "rpc_pcall: send failed" << std::endl;

        std::vector<VariantType> res;
        res.emplace_back(int(XNET_BUFF_LIMIT));
        return RpcCall{ 0, std::move(res)};
    }

    std::vector<VariantType> res;
    res.emplace_back(int(XNET_SUCCESS));
    return RpcCall{pkg_id, std::move(res)};
}

#endif // __XRPC_INL_H__
