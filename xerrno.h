#ifndef XERRNO_H
#define XERRNO_H

// 定义网络错误枚举
enum NetworkError {
    XNET_SUCCESS = 0,
    XNET_CORO_EXCEPT = 1,
    XNET_CORO_FAILED = 2,
    XNET_PROTO_UNKNOWN = 3,
    XNET_MEM_FAIL = 4,
    XNET_NOT_IN_COROUTINE = 5,
    XNET_BUFF_LIMIT = 6,
    XNET_TIMEOUT = 7,
    XNET_INVALID_RESPONSE = 8,
    XNET_SERVER_ERROR,
    XNET_CLIENT_ERROR,
    XNET_UNKNOWN_ERROR
};

#endif
