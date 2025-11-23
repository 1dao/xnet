#ifndef _XRPC_H
#define _XRPC_H

#include "xchannel.h"
#include "xpack.h"
#include "xrpc_awaiter.h"

enum XRpcError {
    XRPC_SUCCESS = 0,
    XRPC_NOT_IN_COROUTINE = -1,
    XRPC_SEND_FAILED = -2,
    XRPC_TIMEOUT = -3,
    XRPC_INVALID_RESPONSE = -4
};

// 检查 RPC 响应
bool xrpc_resp_blp4(xChannel* channel);

// RPC 调用函数声明
template<typename... Args>
xrpc_awaiter xrpc_pcall(xChannel* channel, uint16_t protocol, const Args&... args);
// int xrpc_res_blp4(xChannel* channel, )
#endif // _XRPC_H