// Copyright 2025 The xnet Authors
// Licensed under the Apache License, Version 2.0

#ifndef NATS_INTERNAL_H
#define NATS_INTERNAL_H

// Include the public API first to get error codes and other defines
// nats_client.h contains C++ code, so it must be outside extern "C"
#include "nats_client.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nats_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// All basic types and declarations are defined here for internal use
// We need to repeat these because nats_client.h doesn't export them publicly anymore

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

// 连接选项默认值
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

// 内部使用的旧版本全局初始化
int nats_InitInternal(void);
void nats_Uninit(void);

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

// C++ 头文件放在 extern "C" 块外
#include "../xchannel.h"
#include "../ae.h"
#include "../xtimer.h"

// 内部连接结构
typedef struct natsInternalConn {
    // xnet 集成
    xChannel *channel;      // xnet 通道
    aeEventLoop *el;        // 事件循环 (全局)

    // 状态
    int state;
    int err;
    char errStr[256];

    // 服务器信息
    char *serverId;
    char *serverVersion;
    int64_t maxPayload;
    bool headersSupported;

    // 订阅管理
    struct natsSubMap *subscriptions;  // sid -> natsSubscription
    int64_t nextSid;

    // PING/PONG 管理
    int pingsOut;
    int64_t lastActivity;
    xtimerHandler pingTimer;           // 使用 xtimer 替代 aeTimeEvent

    // 响应订阅 (用于 Request/Reply)
    char *respPrefix;
    struct natsSubscription *respMux;
    struct natsReqMap *respMap;

    // 连接配置
    natsConnOptions opts;

    // 重连状态
    int reconnectAttempts;
    xtimerHandler reconnectTimer;      // 使用 xtimer 替代 aeTimeEvent

    // 协议解析器
    natsParser *parser;

    // 发送缓冲
    char *sendBuf;
    int sendBufSize;
    int sendBufLen;

    // 引用计数
    int refs;

    // 连接锁
    void *mutex;
} natsInternalConn;

// 全局客户端上下文
typedef struct {
    aeEventLoop *el;
    int initCount;
    void *mutex;
    natsConnection *conn;      // 全局连接
    char *nodeName;            // 当前节点名称
} natsGlobalCtx;

// 全局上下文
extern natsGlobalCtx g_nats;

// 内部函数
void natsLock_Init(void);
void natsLock_Uninit(void);
void* natsLock_Create(void);
void natsLock_Destroy(void *lock);
void natsLock_Lock(void *lock);
void natsLock_Unlock(void *lock);

// 连接内部函数
void natsConn_Lock(natsConnection *conn);
void natsConn_Unlock(natsConnection *conn);
void natsConn_setError(natsConnection *conn, int err, const char *errStr);
void natsConn_clearError(natsConnection *conn);

// 传输层回调
int natsConn_onPacket(xChannel *ch, char *data, int len);
int natsConn_onClose(xChannel *ch, char *data, int len);

// 解析器回调
void natsConn_onInfo(void *userdata, const char *json, int len);
void natsConn_onMsg(void *userdata, natsMsgArgs *args, const char *hdrs, int hdrLen, const char *payload, int payloadLen);
void natsConn_onPing(void *userdata);
void natsConn_onPong(void *userdata);
void natsConn_onError(void *userdata, const char *err, int len);
void natsConn_onOK(void *userdata);

// 重连
void natsConn_startReconnect(natsConnection *conn);
void natsConn_doReconnect(natsConnection *conn);

// 发送函数
int natsConn_send(natsConnection *conn, const char *data, int len);
int natsConn_flush(natsConnection *conn);

// 订阅管理
void natsSub_deliverMessage(natsSubscription *sub, natsMessage *msg);

// 工具函数
char* nats_strdup(const char *s);
char* nats_strndup(const char *s, int n);
int nats_parseUrl(const char *url, char *host, int hostSize, int *port);

#endif /* NATS_INTERNAL_H_ */