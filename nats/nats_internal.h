// Copyright 2025 The xnet Authors
// Licensed under the Apache License, Version 2.0

#ifndef NATS_INTERNAL_H
#define NATS_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nats_client.h"
#include "nats_protocol.h"

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
    xChannel *channel;          // xnet 通道
    aeEventLoop *el;            // 事件循环 (全局)

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
    xtimerHandler pingTimer;    // 使用 xtimer 替代 aeTimeEvent

    // 响应订阅 (用于 Request/Reply)
    char *respPrefix;
    struct natsSubscription *respMux;
    struct natsReqMap *respMap;

    // 连接配置
    natsConnOptions opts;

    // 重连状态
    int reconnectAttempts;
    xtimerHandler reconnectTimer;  // 使用 xtimer 替代 aeTimeEvent

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
void natsConn_onMsg(void *userdata, natsMsgArgs *args, const char *hdrs, int hdrLen,
                    const char *payload, int payloadLen);
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
