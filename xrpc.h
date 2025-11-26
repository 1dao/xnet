#ifndef _XRPC_H
#define _XRPC_H

#include "xchannel.h"
#include "xpack.h"

enum xNetErr {
    XNET_SUCCESS = 0,
    XNET_NOT_IN_COROUTINE = -1,
    XNET_BUFF_LIMIT = -2,
    XNET_TIMEOUT = -3,
    XNET_INVALID_RESPONSE = -4
};
struct RpcCall;

// 检查 RPC 响应
bool xrpc_resp_blp4(xChannel* channel);

// RPC 调用函数声明-inl中声明
//template<typename... Args>
//RpcCall xrpc_pcall(xChannel* s, uint16_t protocol, Args&&... args);
int xrpc_resp(xChannel* s, int co_id, uint32_t pkg_id, XPackBuff& res);

#include "xrpc.inl"

#endif // _XRPC_H
