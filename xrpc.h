#ifndef _XRPC_H
#define _XRPC_H

#include "xchannel.h"
#include "xpack.h"

#include <iostream>
#include <utility>
#include <type_traits>
#include "ae.h"
#include "xcoroutine.h"
#include "xchannel.inl"
#include "xerrno.h"

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

// 函数声明 - POST 模式
template<typename... Args>
NetworkError xchannel_post(xChannel* s, uint16_t protocol, Args&&... args) {
    uint16_t is_rpc = 0;
    XPackBuff packed = xpack_pack(true, std::forward<Args>(args)...);

    int remain = (int)s->wlen - (int)(s->wpos - s->wbuf);
    int hlen = (int)_xchannel_header_size(s);
    int plen = packed.len + sizeof(is_rpc) + sizeof(protocol);
    if (remain < hlen + plen)
        return XNET_BUFF_LIMIT;
    _xchannel_write_header(s, plen);

    // 写RPC头
    *(uint16_t*)s->wpos = htons(is_rpc);
    s->wpos += sizeof(is_rpc);
    *(uint16_t*)s->wpos = htons(protocol);
    s->wpos += sizeof(protocol);

    xchannel_rawsend(s, packed.get(), packed.len);
    return XNET_SUCCESS;
}

// 检查 RPC 结果的辅助宏/函数
#define XRPC_CHECK_RETURN(result, msg) \
    do { \
        if (result.empty()) { \
            xlog_err("%s: empty result", msg); \
            co_return; \
        } \
        int _retcode = std::get<int>(result[0]); \
        if (_retcode != 0) { \
            xlog_err("%s failed, retcode: %d", msg, _retcode); \
            co_return; \
        } \
    } while(0)

// 获取 retcode
inline int xrpc_retcode(const std::vector<VariantType>& result) {
    if (result.empty()) return -999;
    return std::get<int>(result[0]);
}

// 检查是否成功
inline bool xrpc_ok(const std::vector<VariantType>& result) {
    return !result.empty() && std::get<int>(result[0]) == 0;
}

int _xrpc_resp(xChannel* s, int co_id, uint32_t wait_id, int retcode, XPackBuff& res);

inline int _xrpc_resp_ok(xChannel* s, int co_id, uint32_t wait_id, XPackBuff& res) {
    return _xrpc_resp(s, co_id, wait_id, 0, res);
}

inline int _xrpc_resp_err(xChannel* s, int co_id, uint32_t wait_id, int errcode) {
    XPackBuff empty;
    return _xrpc_resp(s, co_id, wait_id, errcode, empty);
}

#endif // _XRPC_H
