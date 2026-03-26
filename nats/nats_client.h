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

// 前向声明
typedef struct natsClient natsClient;
typedef struct natsConnection natsConnection;
typedef struct natsSubscription natsSubscription;
typedef struct natsMessage natsMessage;

// 回调类型定义
typedef void (*natsMsgHandler)(natsConnection *conn, natsSubscription *sub,
                               natsMessage *msg, void *userdata);
typedef void (*natsConnHandler)(natsConnection *conn, int event, void *userdata);
typedef void (*natsErrHandler)(natsConnection *conn, int err, const char *errStr, void *userdata);

// 消息结构
struct natsMessage {
    char *subject;          // 主题
    char *reply;            // 回复主题 (可能为 NULL)
    char *data;             // 消息体
    int   dataLen;          // 消息体长度

    // Headers (NATS 2.0+)
    char **hdrKeys;
    char **hdrVals;
    int    hdrCount;

    // 内部使用
    natsConnection *conn;
    int64_t sid;            // 订阅ID
    int     refs;           // 引用计数
};

// 订阅结构
struct natsSubscription {
    int64_t sid;            // 订阅ID
    char *subject;          // 订阅主题
    char *queue;            // 队列组 (可能为 NULL)

    natsMsgHandler handler; // 消息回调
    void *userdata;         // 用户数据

    // 内部
    natsConnection *conn;
    uint64_t delivered;     // 已投递计数
    uint64_t maxMsgs;       // 最大消息数 (0 = 无限制)
    bool     closed;        // 是否已关闭
};

// 连接配置
typedef struct {
    // 服务器地址
    char *url;              // 服务器地址，如 "nats://localhost:4222"
    char **servers;         // 集群地址列表
    int serverCount;

    // 认证
    char *user;
    char *password;
    char *token;
    char *jwt;
    char *nkeySeed;         // NKey 种子

    // 连接选项
    char *name;             // 客户端名称
    bool verbose;           // 详细模式
    int64_t timeout;        // 连接超时(ms)，默认 2000
    int64_t pingInterval;   // 心跳间隔(ms)，默认 120000
    int maxPingsOut;        // 最大未响应PING数，默认 2

    // 重连选项
    bool allowReconnect;
    int maxReconnect;       // 最大重连次数，默认 60
    int64_t reconnectWait;  // 重连等待时间(ms)，默认 2500

    // 事件回调
    natsConnHandler onConnect;
    natsConnHandler onDisconnect;
    natsConnHandler onReconnect;
    natsErrHandler onError;
    void *userdata;
} natsConnOptions;

// 连接结构 (部分公开)
struct natsConnection {
    // 状态
    int state;              // 连接状态
    int err;                // 最后错误码
    char errStr[256];       // 错误描述

    // 服务器信息
    char *serverId;
    char *serverVersion;
    int64_t maxPayload;
    bool headersSupported;

    // 配置
    natsConnOptions opts;

    // 引用计数
    int refs;

    // 内部数据 (不透明指针)
    void *internal;
};

// 全局初始化/清理
int nats_Init(void);
void nats_Uninit(void);

// 连接选项
#define natsConnOptions_Defaults() { \
    .url = NULL, \
    .servers = NULL, \
    .serverCount = 0, \
    .user = NULL, \
    .password = NULL, \
    .token = NULL, \
    .jwt = NULL, \
    .nkeySeed = NULL, \
    .name = NULL, \
    .verbose = false, \
    .timeout = 2000, \
    .pingInterval = 120000, \
    .maxPingsOut = 2, \
    .allowReconnect = true, \
    .maxReconnect = 60, \
    .reconnectWait = 2500, \
    .onConnect = NULL, \
    .onDisconnect = NULL, \
    .onReconnect = NULL, \
    .onError = NULL, \
    .userdata = NULL \
}

// 连接管理
natsConnection* natsConnection_Connect(const char *url);
natsConnection* natsConnection_ConnectWithOptions(natsConnOptions *opts);
void natsConnection_Close(natsConnection *conn);
void natsConnection_Destroy(natsConnection *conn);

// 连接状态
int natsConnection_GetState(natsConnection *conn);
bool natsConnection_IsConnected(natsConnection *conn);
bool natsConnection_IsClosed(natsConnection *conn);
const char* natsConnection_GetLastError(natsConnection *conn);

// 发布消息
int natsConnection_Publish(natsConnection *conn, const char *subject,
                            const char *data, int dataLen);
int natsConnection_PublishString(natsConnection *conn, const char *subject,
                                  const char *str);
int natsConnection_PublishWithReply(natsConnection *conn, const char *subject,
                                     const char *reply, const char *data, int dataLen);
int natsConnection_Flush(natsConnection *conn);
int natsConnection_FlushTimeout(natsConnection *conn, int64_t timeout);

// 订阅管理
natsSubscription* natsConnection_Subscribe(natsConnection *conn, const char *subject,
                                           natsMsgHandler handler, void *userdata);
natsSubscription* natsConnection_SubscribeSync(natsConnection *conn, const char *subject);
natsSubscription* natsConnection_QueueSubscribe(natsConnection *conn, const char *subject,
                                                 const char *queue, natsMsgHandler handler,
                                                 void *userdata);
int natsSubscription_Unsubscribe(natsSubscription *sub);
int natsSubscription_AutoUnsubscribe(natsSubscription *sub, int maxMsgs);
int natsSubscription_SetPendingLimits(natsSubscription *sub, int msgLimit, int bytesLimit);

// 同步订阅
natsMessage* natsSubscription_NextMsg(natsSubscription *sub, int64_t timeout);

// 请求-响应
natsMessage* natsConnection_Request(natsConnection *conn, const char *subject,
                                     const char *data, int dataLen, int64_t timeout);
int natsConnection_RequestAsync(natsConnection *conn, const char *subject,
                                 const char *data, int dataLen,
                                 natsMsgHandler handler, void *userdata);

// 消息操作
const char* natsMessage_GetSubject(natsMessage *msg);
const char* natsMessage_GetReply(natsMessage *msg);
const char* natsMessage_GetData(natsMessage *msg);
int natsMessage_GetDataLength(natsMessage *msg);
const char* natsMessage_GetHeader(natsMessage *msg, const char *key);
int natsMessage_GetHeaders(natsMessage *msg, char ***keys, char ***vals, int *count);

// 回复消息
int natsMessage_Reply(natsMessage *msg, const char *data, int dataLen);
int natsMessage_ReplyString(natsMessage *msg, const char *str);

// 消息引用计数
void natsMessage_Retain(natsMessage *msg);
void natsMessage_Destroy(natsMessage *msg);

// 生成 inbox 主题
char* natsConnection_NewInbox(natsConnection *conn);

// 错误码转字符串
const char* natsStatus_GetText(int status);

#ifdef __cplusplus
}
#endif

#endif /* NATS_CLIENT_H_ */
