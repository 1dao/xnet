// Copyright 2025 The xnet Authors
// Licensed under the Apache License, Version 2.0

#ifndef NATS_CLIENT_H
#define NATS_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 版本信息
#define NATS_VERSION_MAJOR  1
#define NATS_VERSION_MINOR  0
#define NATS_VERSION_PATCH  0

// 错误码
#define NATS_OK                     0
#define NATS_ERR                   -1
#define NATS_ERR_INVALID_ARG       -2
#define NATS_ERR_NO_MEMORY         -3
#define NATS_ERR_CONNECTION_CLOSED -4
#define NATS_ERR_CONNECTION_LOST   -5
#define NATS_ERR_TIMEOUT           -6
#define NATS_ERR_PROTOCOL          -7
#define NATS_ERR_AUTH_FAILED       -8
#define NATS_ERR_MAX_PAYLOAD       -9
#define NATS_ERR_SUBSCRIPTION_CLOSED -10
#define NATS_ERR_NO_RESPONDERS     -11
#define NATS_ERR_NOT_IN_COROUTINE  -12
#define NATS_ERR_NOT_INITIALIZED   -13

// 连接状态
#define NATS_STATE_DISCONNECTED     0
#define NATS_STATE_CONNECTING       1
#define NATS_STATE_CONNECTED        2
#define NATS_STATE_RECONNECTING     3
#define NATS_STATE_CLOSED           4

// 事件类型
#define NATS_EVENT_CONNECTED        1
#define NATS_EVENT_DISCONNECTED     2
#define NATS_EVENT_RECONNECTED      3
#define NATS_EVENT_CLOSED           4
#define NATS_EVENT_ERROR            5

// ============================================================================
// 简化版 API - 初始化/启动模式
// ============================================================================

// 简化版 API 配置结构
typedef struct {
    const char *node_name;   // 本进程名称（用于 RPC 路由），例如 "game1"
    const char *url;         // NATS 服务器地址，如 "nats://localhost:4222"
    void (*on_connected)(void);   // 连接成功时的回调
    void (*on_disconnected)(void); // 连接断开时的回调
    void (*on_error)(int err, const char *err_str); // 错误回调
} natsSimplifiedOpts;

// 初始化简化版 API（配置阶段）
// 参数 opts: 配置信息，不可为 NULL
// 返回: NATS_OK 成功，否则错误码
int nats_Init(const natsSimplifiedOpts *opts);

// 启动连接并进入事件循环（阻塞直到调用 nats_Stop）
void nats_Start(void);

// 停止连接并清理资源
void nats_Stop(void);

// ============================================================================
// 底层 API - 需要显式传递 conn 参数（仅供内部使用，外部不应调用）
// ============================================================================

// 前向声明
typedef struct natsConnection natsConnection;
typedef struct natsSubscription natsSubscription;
typedef struct natsMessage natsMessage;

#ifdef __cplusplus
} // close extern "C"
#endif

// C++ only: Protocol handlers and VRPC request API
#ifdef __cplusplus
#include <vector>
#include "../xpack.h"
#include "../xcoroutine.h"

// 定义协议处理函数类型
typedef int (*natsPostHandler)(natsConnection *conn, std::vector<VariantType>& args);
typedef XPackBuff (*natsVRPCHandler)(natsConnection *conn, std::vector<VariantType>& args);

// 注册处理函数
void nats_reg_publish(int pt, natsPostHandler h);  // 处理 server 间广播消息
void nats_reg_vrpc(int pt, natsVRPCHandler h);     // 处理 server 间 RPC 请求

// 发送 VRPC 请求到目标节点（co_await 版本）
// target - 目标进程名称 (e.g. "game2")
// pt - 协议类型编号
// data/dataLen - 打包后的参数数据
struct xAwaiter nats_rpc(const char *target, int pt,
                                const char *data, int dataLen,
                                int64_t timeout_ms = 10000);

// 底层 VRPC 请求 API - 带连接参数
struct xAwaiter nats_raw_rpc(natsConnection *conn, const char *subject,
                                                     int pt, const char *data, int dataLen,
                                                     int64_t timeout_ms = 10000);

#endif // __cplusplus

#endif /* NATS_CLIENT_H_ */
