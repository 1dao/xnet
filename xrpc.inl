#ifndef __XRPC_INL_H__
#define __XRPC_INL_H__

#include <iostream>
#include <utility>
#include <type_traits>

#include "ae.h"
#include "xcoroutine.h"
#include "xchannel.inl"

// 直接发包并返回 RpcCall（pkg_id 用协程 id）
template<typename... Args>
xAwaiter xrpc_pcall(xChannel* s, uint16_t protocol, Args&&... args) {
    xAwaiter awaiter;
    uint32_t wait_id = awaiter.wait_id();
    if (wait_id == 0) {
        return xAwaiter(XNET_NOT_IN_COROUTINE);
    }

    int co_id = coroutine_self_id();
    if (co_id == -1) {
        return xAwaiter(XNET_NOT_IN_COROUTINE);
    }

    uint16_t is_rpc = 1;
    XPackBuff packed = xpack_pack(true, std::forward<Args>(args)...);

    int remain = (int)s->wlen - (int)(s->wpos - s->wbuf);
    int hlen = (int)_xchannel_header_size(s);
    int plen = packed.len + sizeof(wait_id) + sizeof(co_id) + sizeof(is_rpc) + sizeof(protocol);

    if (remain < hlen + plen) {
        return xAwaiter(XNET_BUFF_LIMIT);
    }
    _xchannel_write_header(s, plen);

    // 写RPC头
    *(uint16_t*)s->wpos = htons(is_rpc);
    s->wpos += sizeof(is_rpc);
    *(uint32_t*)s->wpos = htonl(wait_id);
    s->wpos += sizeof(wait_id);
    *(int*)s->wpos = htonl(co_id);
    s->wpos += sizeof(co_id);
    *(uint16_t*)s->wpos = htons(protocol);
    s->wpos += sizeof(protocol);

    int sendlen = xchannel_rawsend(s, packed.get(), packed.len);
    if (sendlen != packed.len)
        return xAwaiter(XNET_BUFF_LIMIT);
    else
        return awaiter;
}

#endif // __XRPC_INL_H__
