// Copyright 2025 The xnet Authors
// Licensed under the Apache License, Version 2.0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "nats_internal.h"
#include "nats_protocol.h"

// 全局上下文
natsGlobalCtx g_nats = {0};

// 简单的互斥锁实现
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION natsMutex;
#else
#include <pthread.h>
typedef pthread_mutex_t natsMutex;
#endif

void natsLock_Init(void) {}
void natsLock_Uninit(void) {}

void* natsLock_Create(void) {
    natsMutex *m = (natsMutex*)malloc(sizeof(natsMutex));
    if (!m) return NULL;
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
    return m;
}

void natsLock_Destroy(void *lock) {
    if (!lock) return;
    natsMutex *m = (natsMutex*)lock;
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
    free(m);
}

void natsLock_Lock(void *lock) {
    if (!lock) return;
    natsMutex *m = (natsMutex*)lock;
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}

void natsLock_Unlock(void *lock) {
    if (!lock) return;
    natsMutex *m = (natsMutex*)lock;
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

// 全局初始化 - 使用全局事件循环，不再自己创建
int nats_Init(void) {
    if (g_nats.initCount++ > 0) return NATS_OK;

    // 获取全局事件循环（由应用层创建）
    g_nats.el = aeGetCurEventLoop();
    if (!g_nats.el) {
        g_nats.initCount--;
        return NATS_ERR;
    }

    g_nats.mutex = natsLock_Create();
    if (!g_nats.mutex) {
        g_nats.el = NULL;
        g_nats.initCount--;
        return NATS_ERR;
    }

    return NATS_OK;
}

void nats_Uninit(void) {
    if (--g_nats.initCount > 0) return;

    // 不删除事件循环，因为那是全局的
    g_nats.el = NULL;

    if (g_nats.mutex) {
        natsLock_Destroy(g_nats.mutex);
        g_nats.mutex = NULL;
    }
}

// 订阅映射表
#define SUB_MAP_SIZE 64

typedef struct natsSubMapEntry {
    int64_t sid;
    natsSubscription *sub;
    struct natsSubMapEntry *next;
} natsSubMapEntry;

typedef struct natsSubMap {
    natsSubMapEntry *buckets[SUB_MAP_SIZE];
    void *mutex;
} natsSubMap;

static natsSubMap* subMap_Create(void) {
    natsSubMap *m = (natsSubMap*)calloc(1, sizeof(natsSubMap));
    if (m) m->mutex = natsLock_Create();
    return m;
}

static void subMap_Destroy(natsSubMap *m) {
    if (!m) return;
    for (int i = 0; i < SUB_MAP_SIZE; i++) {
        natsSubMapEntry *e = m->buckets[i];
        while (e) {
            natsSubMapEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    natsLock_Destroy(m->mutex);
    free(m);
}

static int subMap_hash(int64_t sid) { return (int)(sid % SUB_MAP_SIZE); }

static void subMap_Set(natsSubMap *m, int64_t sid, natsSubscription *sub) {
    if (!m) return;
    natsLock_Lock(m->mutex);
    int h = subMap_hash(sid);
    natsSubMapEntry *e = m->buckets[h];
    while (e) {
        if (e->sid == sid) {
            e->sub = sub;
            natsLock_Unlock(m->mutex);
            return;
        }
        e = e->next;
    }
    e = (natsSubMapEntry*)malloc(sizeof(natsSubMapEntry));
    e->sid = sid;
    e->sub = sub;
    e->next = m->buckets[h];
    m->buckets[h] = e;
    natsLock_Unlock(m->mutex);
}

static natsSubscription* subMap_Get(natsSubMap *m, int64_t sid) {
    if (!m) return NULL;
    natsLock_Lock(m->mutex);
    int h = subMap_hash(sid);
    natsSubMapEntry *e = m->buckets[h];
    while (e) {
        if (e->sid == sid) {
            natsSubscription *sub = e->sub;
            natsLock_Unlock(m->mutex);
            return sub;
        }
        e = e->next;
    }
    natsLock_Unlock(m->mutex);
    return NULL;
}

static void subMap_Delete(natsSubMap *m, int64_t sid) {
    if (!m) return;
    natsLock_Lock(m->mutex);
    int h = subMap_hash(sid);
    natsSubMapEntry **pp = &m->buckets[h];
    while (*pp) {
        natsSubMapEntry *e = *pp;
        if (e->sid == sid) {
            *pp = e->next;
            free(e);
            natsLock_Unlock(m->mutex);
            return;
        }
        pp = &e->next;
    }
    natsLock_Unlock(m->mutex);
}

// 连接锁操作
void natsConn_Lock(natsConnection *conn) {
    if (conn && conn->internal) {
        natsInternalConn *ic = (natsInternalConn*)conn->internal;
        natsLock_Lock(ic->mutex);
    }
}

void natsConn_Unlock(natsConnection *conn) {
    if (conn && conn->internal) {
        natsInternalConn *ic = (natsInternalConn*)conn->internal;
        natsLock_Unlock(ic->mutex);
    }
}

void natsConn_setError(natsConnection *conn, int err, const char *errStr) {
    if (!conn) return;
    conn->err = err;
    if (errStr) {
        strncpy(conn->errStr, errStr, sizeof(conn->errStr) - 1);
        conn->errStr[sizeof(conn->errStr) - 1] = '\0';
    } else {
        conn->errStr[0] = '\0';
    }
}

void natsConn_clearError(natsConnection *conn) {
    if (!conn) return;
    conn->err = NATS_OK;
    conn->errStr[0] = '\0';
}

// 工具函数
char* nats_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = (char*)malloc(len + 1);
    if (dup) memcpy(dup, s, len + 1);
    return dup;
}

char* nats_strndup(const char *s, int n) {
    if (!s) return NULL;
    char *dup = (char*)malloc(n + 1);
    if (dup) {
        memcpy(dup, s, n);
        dup[n] = '\0';
    }
    return dup;
}

int nats_parseUrl(const char *url, char *host, int hostSize, int *port) {
    if (!url || !host || !port) return -1;
    const char *p = url;
    *port = 4222;
    if (strncmp(p, "nats://", 7) == 0) p += 7;
    else if (strncmp(p, "tls://", 6) == 0) p += 6;
    const char *colon = strchr(p, ':');
    if (colon) {
        int hostLen = colon - p;
        if (hostLen >= hostSize) hostLen = hostSize - 1;
        memcpy(host, p, hostLen);
        host[hostLen] = '\0';
        *port = atoi(colon + 1);
    } else {
        strncpy(host, p, hostSize - 1);
        host[hostSize - 1] = '\0';
    }
    return 0;
}

// 解析器回调
void natsConn_onInfo(void *userdata, const char *json, int len) {
    natsConnection *conn = (natsConnection*)userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    char serverId[256] = {0}, serverVersion[64] = {0};
    int64_t maxPayload = 1048576;
    bool headersSupported = false;

    natsProto_ParseInfo(json, len, serverId, sizeof(serverId),
                        serverVersion, sizeof(serverVersion),
                        &maxPayload, &headersSupported);

    natsConn_Lock(conn);
    if (conn->serverId) free(conn->serverId);
    if (conn->serverVersion) free(conn->serverVersion);
    conn->serverId = nats_strdup(serverId);
    conn->serverVersion = nats_strdup(serverVersion);
    conn->maxPayload = maxPayload;
    conn->headersSupported = headersSupported;

    char connectBuf[4096];
    int n = natsProto_EncodeConnect(connectBuf, sizeof(connectBuf),
                                     ic->opts.name, ic->opts.verbose, false,
                                     ic->opts.user, ic->opts.password, ic->opts.token,
                                     ic->opts.jwt, ic->opts.nkeySeed, true, true);
    if (n > 0) {
        xchannel_rawsend(ic->channel, connectBuf, n);
        xchannel_flush(ic->channel);
    }

    char pingBuf[16];
    n = natsProto_EncodePing(pingBuf, sizeof(pingBuf));
    if (n > 0) {
        xchannel_rawsend(ic->channel, pingBuf, n);
        xchannel_flush(ic->channel);
    }
    natsConn_Unlock(conn);
}

void natsConn_onMsg(void *userdata, natsMsgArgs *args, const char *hdrs, int hdrLen,
                    const char *payload, int payloadLen) {
    natsConnection *conn = (natsConnection*)userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsSubscription *sub = subMap_Get(ic->subscriptions, args->sid);
    if (!sub) return;

    natsMessage *msg = (natsMessage*)calloc(1, sizeof(natsMessage));
    if (!msg) return;

    msg->subject = nats_strdup(args->subject);
    msg->reply = args->reply[0] ? nats_strdup(args->reply) : NULL;
    msg->data = (char*)malloc(payloadLen + 1);
    if (msg->data) {
        memcpy(msg->data, payload, payloadLen);
        msg->data[payloadLen] = '\0';
    }
    msg->dataLen = payloadLen;
    msg->conn = conn;
    msg->sid = args->sid;
    msg->refs = 1;

    sub->delivered++;
    if (sub->handler) sub->handler(conn, sub, msg, sub->userdata);
    natsMessage_Destroy(msg);

    if (sub->maxMsgs > 0 && sub->delivered >= sub->maxMsgs) {
        natsSubscription_Unsubscribe(sub);
    }
}

void natsConn_onPing(void *userdata) {
    natsConnection *conn = (natsConnection*)userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    char pongBuf[16];
    int n = natsProto_EncodePong(pongBuf, sizeof(pongBuf));
    if (n > 0) xchannel_rawsend(ic->channel, pongBuf, n);
}

void natsConn_onPong(void *userdata) {
    natsConnection *conn = (natsConnection*)userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsConn_Lock(conn);
    ic->pingsOut = 0;
    // 收到 PONG 表示连接已建立
    if (conn->state == NATS_STATE_CONNECTING) {
        conn->state = NATS_STATE_CONNECTED;
        if (ic->opts.onConnect)
            ic->opts.onConnect(conn, NATS_EVENT_CONNECTED, ic->opts.userdata);
    }
    natsConn_Unlock(conn);
}

void natsConn_onError(void *userdata, const char *err, int len) {
    natsConnection *conn = (natsConnection*)userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    char *errStr = nats_strndup(err, len);
    natsConn_setError(conn, NATS_ERR, errStr);
    if (ic->opts.onError) ic->opts.onError(conn, NATS_ERR, errStr, ic->opts.userdata);
    free(errStr);
}

void natsConn_onOK(void *userdata) {}

// xChannel 回调
int natsConn_onPacket(xChannel *ch, char *data, int len) {
    natsConnection *conn = (natsConnection*)ch->userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsParser_Parse(ic->parser, data, len);
    return len;
}

int natsConn_onClose(xChannel *ch, char *data, int len) {
    (void)data;
    (void)len;
    natsConnection *conn = (natsConnection*)ch->userdata;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsConn_Lock(conn);
    if (conn->state != NATS_STATE_CLOSED) {
        conn->state = NATS_STATE_DISCONNECTED;
        natsConn_setError(conn, NATS_ERR_CONNECTION_LOST, "connection closed");
        if (ic->opts.onDisconnect)
            ic->opts.onDisconnect(conn, NATS_EVENT_DISCONNECTED, ic->opts.userdata);
        if (ic->opts.allowReconnect && ic->reconnectAttempts < ic->opts.maxReconnect)
            natsConn_startReconnect(conn);
    }
    natsConn_Unlock(conn);
    return 0;
}

// 定时器回调包装器 - 使用 xtimer
typedef struct {
    natsConnection *conn;
    xtimerHandler timer;
} natsTimerData;

// 心跳定时器回调
static void natsConn_pingTimerCallback(void *clientData) {
    natsConnection *conn = (natsConnection*)clientData;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsConn_Lock(conn);
    if (conn->state != NATS_STATE_CONNECTED) {
        natsConn_Unlock(conn);
        return;
    }
    if (ic->pingsOut >= ic->opts.maxPingsOut) {
        natsConn_setError(conn, NATS_ERR_CONNECTION_LOST, "ping timeout");
        natsConn_Unlock(conn);
        xchannel_close(ic->channel);
        return;
    }
    char pingBuf[16];
    int n = natsProto_EncodePing(pingBuf, sizeof(pingBuf));
    if (n > 0) {
        xchannel_rawsend(ic->channel, pingBuf, n);
        ic->pingsOut++;
    }
    natsConn_Unlock(conn);
}

// 重连定时器回调
static void natsConn_reconnectTimerCallback(void *clientData);

// 启动心跳定时器
static void natsConn_startPingTimer(natsConnection *conn) {
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (ic->pingTimer) return;
    ic->pingTimer = xtimer_add((int)ic->opts.pingInterval, "nats_ping",
                               natsConn_pingTimerCallback, conn, -1);
}

// 停止心跳定时器
static void natsConn_stopPingTimer(natsConnection *conn) {
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (ic->pingTimer) {
        xtimer_del(ic->pingTimer);
        ic->pingTimer = NULL;
    }
}

// 重连逻辑
void natsConn_startReconnect(natsConnection *conn) {
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (ic->reconnectTimer) return;
    conn->state = NATS_STATE_RECONNECTING;
    ic->reconnectAttempts = 0;
    ic->reconnectTimer = xtimer_add((int)ic->opts.reconnectWait, "nats_reconnect",
                                    natsConn_reconnectTimerCallback, conn, -1);
}

static void natsConn_reconnectTimerCallback(void *clientData) {
    natsConnection *conn = (natsConnection*)clientData;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsConn_Lock(conn);
    if (conn->state == NATS_STATE_CLOSED) {
        natsConn_Unlock(conn);
        ic->reconnectTimer = NULL;
        return;
    }
    if (++ic->reconnectAttempts > ic->opts.maxReconnect) {
        natsConn_setError(conn, NATS_ERR, "max reconnect attempts reached");
        natsConn_Unlock(conn);
        ic->reconnectTimer = NULL;
        return;
    }
    natsConn_Unlock(conn);
    natsConn_doReconnect(conn);
    natsConn_Lock(conn);
    if (conn->state == NATS_STATE_CONNECTED) {
        natsConn_Unlock(conn);
        ic->reconnectTimer = NULL;
        return;
    }
    natsConn_Unlock(conn);
}

void natsConn_doReconnect(natsConnection *conn) {
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (ic->channel) {
        xchannel_close(ic->channel);
        ic->channel = NULL;
    }
    char host[256];
    int port;
    const char *url = ic->opts.url;
    if (!url && ic->opts.servers && ic->opts.serverCount > 0)
        url = ic->opts.servers[0];
    if (!url || nats_parseUrl(url, host, sizeof(host), &port) < 0) return;
    xChannel *ch = xchannel_conn(host, port, natsConn_onPacket, natsConn_onClose,
                                  conn, xrpoto_crlf_http1);
    if (!ch) return;
    natsConn_Lock(conn);
    ic->channel = ch;
    conn->state = NATS_STATE_CONNECTING;
    natsParser_Reset(ic->parser);
    natsConn_Unlock(conn);
}

// 发送函数
int natsConn_send(natsConnection *conn, const char *data, int len) {
    if (!conn || !conn->internal) return NATS_ERR_INVALID_ARG;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (!ic->channel) return NATS_ERR_CONNECTION_CLOSED;
    int ret = xchannel_rawsend(ic->channel, data, len);
    return (ret >= 0) ? NATS_OK : NATS_ERR;
}

int natsConn_flush(natsConnection *conn) {
    if (!conn || !conn->internal) return NATS_ERR_INVALID_ARG;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (!ic->channel) return NATS_ERR_CONNECTION_CLOSED;
    int ret = xchannel_flush(ic->channel);
    return (ret >= 0) ? NATS_OK : NATS_ERR;
}

// ==================== 对外 API 实现 ====================

natsConnection* natsConnection_ConnectWithOptions(natsConnOptions *opts) {
    if (!opts) return NULL;

    // 确保已初始化
    if (g_nats.initCount == 0) {
        if (nats_Init() != NATS_OK) return NULL;
    }

    natsConnection *conn = (natsConnection*)calloc(1, sizeof(natsConnection));
    if (!conn) return NULL;

    natsInternalConn *ic = (natsInternalConn*)calloc(1, sizeof(natsInternalConn));
    if (!ic) {
        free(conn);
        return NULL;
    }

    conn->internal = ic;
    ic->el = g_nats.el;
    ic->opts = *opts;
    if (opts->url) ic->opts.url = nats_strdup(opts->url);
    ic->mutex = natsLock_Create();
    ic->subscriptions = subMap_Create();
    ic->nextSid = 1;
    ic->pingsOut = 0;
    ic->parser = natsParser_Create(conn);
    if (!ic->parser) {
        natsLock_Destroy(ic->mutex);
        free(ic);
        free(conn);
        return NULL;
    }

    // 设置解析器回调
    natsParser_SetCallbacks(ic->parser,
        natsConn_onInfo,
        natsConn_onMsg,
        natsConn_onPing,
        natsConn_onPong,
        natsConn_onError,
        natsConn_onOK);

    // 解析服务器地址并连接
    char host[256];
    int port;
    const char *url = ic->opts.url;
    if (!url && ic->opts.servers && ic->opts.serverCount > 0)
        url = ic->opts.servers[0];

    if (!url || nats_parseUrl(url, host, sizeof(host), &port) < 0) {
        natsParser_Destroy(ic->parser);
        natsLock_Destroy(ic->mutex);
        free(ic);
        free(conn);
        return NULL;
    }

    conn->state = NATS_STATE_CONNECTING;
    xChannel *ch = xchannel_conn(host, port, natsConn_onPacket, natsConn_onClose,
                                  conn, xrpoto_crlf_http1);
    if (!ch) {
        natsConn_setError(conn, NATS_ERR, "connection failed");
        natsParser_Destroy(ic->parser);
        subMap_Destroy(ic->subscriptions);
        natsLock_Destroy(ic->mutex);
        if (ic->opts.url) free(ic->opts.url);
        free(ic);
        free(conn);
        return NULL;
    }

    ic->channel = ch;

    // 启动心跳定时器
    natsConn_startPingTimer(conn);

    conn->refs = 1;
    return conn;
}

natsConnection* natsConnection_Connect(const char *url) {
    natsConnOptions opts = natsConnOptions_Defaults();
    opts.url = (char*)url;
    return natsConnection_ConnectWithOptions(&opts);
}

void natsConnection_Close(natsConnection *conn) {
    if (!conn) return;
    natsConn_Lock(conn);
    if (conn->state == NATS_STATE_CLOSED) {
        natsConn_Unlock(conn);
        return;
    }
    conn->state = NATS_STATE_CLOSED;
    natsInternalConn *ic = (natsInternalConn*)conn->internal;

    // 停止定时器
    natsConn_stopPingTimer(conn);
    if (ic->reconnectTimer) {
        xtimer_del(ic->reconnectTimer);
        ic->reconnectTimer = NULL;
    }

    if (ic->channel) {
        xchannel_close(ic->channel);
        ic->channel = NULL;
    }
    natsConn_Unlock(conn);
}

void natsConnection_Destroy(natsConnection *conn) {
    if (!conn) return;
    natsConnection_Close(conn);

    // 释放资源
    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    if (ic) {
        if (ic->parser) natsParser_Destroy(ic->parser);
        if (ic->subscriptions) subMap_Destroy(ic->subscriptions);
        if (ic->mutex) natsLock_Destroy(ic->mutex);
        if (ic->opts.url) free(ic->opts.url);
        if (conn->serverId) free(conn->serverId);
        if (conn->serverVersion) free(conn->serverVersion);
        free(ic);
    }
    free(conn);
}

int natsConnection_GetState(natsConnection *conn) {
    if (!conn) return NATS_STATE_CLOSED;
    return conn->state;
}

bool natsConnection_IsConnected(natsConnection *conn) {
    if (!conn) return false;
    return conn->state == NATS_STATE_CONNECTED;
}

bool natsConnection_IsClosed(natsConnection *conn) {
    if (!conn) return true;
    return conn->state == NATS_STATE_CLOSED;
}

const char* natsConnection_GetLastError(natsConnection *conn) {
    if (!conn) return "null connection";
    return conn->errStr[0] ? conn->errStr : "no error";
}

int natsConnection_Publish(natsConnection *conn, const char *subject,
                            const char *data, int dataLen) {
    if (!conn || !subject) return NATS_ERR_INVALID_ARG;
    if (!natsConnection_IsConnected(conn)) return NATS_ERR_CONNECTION_CLOSED;

    char buf[4096];
    int n = natsProto_EncodePub(buf, sizeof(buf), subject, NULL, data, dataLen);
    if (n <= 0) return NATS_ERR;

    return natsConn_send(conn, buf, n);
}

int natsConnection_PublishString(natsConnection *conn, const char *subject,
                                  const char *str) {
    if (!str) return NATS_ERR_INVALID_ARG;
    return natsConnection_Publish(conn, subject, str, (int)strlen(str));
}

int natsConnection_PublishWithReply(natsConnection *conn, const char *subject,
                                     const char *reply, const char *data, int dataLen) {
    if (!conn || !subject) return NATS_ERR_INVALID_ARG;
    if (!natsConnection_IsConnected(conn)) return NATS_ERR_CONNECTION_CLOSED;

    char buf[4096];
    int n = natsProto_EncodePub(buf, sizeof(buf), subject, reply, data, dataLen);
    if (n <= 0) return NATS_ERR;

    return natsConn_send(conn, buf, n);
}

int natsConnection_Flush(natsConnection *conn) {
    return natsConn_flush(conn);
}

natsSubscription* natsConnection_Subscribe(natsConnection *conn, const char *subject,
                                           natsMsgHandler handler, void *userdata) {
    if (!conn || !subject) return NULL;
    if (!natsConnection_IsConnected(conn)) return NULL;

    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsSubscription *sub = (natsSubscription*)calloc(1, sizeof(natsSubscription));
    if (!sub) return NULL;

    sub->sid = ic->nextSid++;
    sub->subject = nats_strdup(subject);
    sub->handler = handler;
    sub->userdata = userdata;
    sub->conn = conn;
    sub->maxMsgs = 0;

    subMap_Set(ic->subscriptions, sub->sid, sub);

    // 发送 SUB 协议
    char buf[1024];
    int n = natsProto_EncodeSub(buf, sizeof(buf), subject, NULL, sub->sid);
    if (n > 0) natsConn_send(conn, buf, n);

    return sub;
}

natsSubscription* natsConnection_QueueSubscribe(natsConnection *conn, const char *subject,
                                                 const char *queue, natsMsgHandler handler,
                                                 void *userdata) {
    if (!conn || !subject || !queue) return NULL;
    if (!natsConnection_IsConnected(conn)) return NULL;

    natsInternalConn *ic = (natsInternalConn*)conn->internal;
    natsSubscription *sub = (natsSubscription*)calloc(1, sizeof(natsSubscription));
    if (!sub) return NULL;

    sub->sid = ic->nextSid++;
    sub->subject = nats_strdup(subject);
    sub->queue = nats_strdup(queue);
    sub->handler = handler;
    sub->userdata = userdata;
    sub->conn = conn;
    sub->maxMsgs = 0;

    subMap_Set(ic->subscriptions, sub->sid, sub);

    // 发送 SUB 协议
    char buf[1024];
    int n = natsProto_EncodeSub(buf, sizeof(buf), subject, queue, sub->sid);
    if (n > 0) natsConn_send(conn, buf, n);

    return sub;
}

int natsSubscription_Unsubscribe(natsSubscription *sub) {
    if (!sub) return NATS_ERR_INVALID_ARG;
    if (sub->closed) return NATS_OK;

    sub->closed = true;
    if (sub->conn && sub->conn->internal) {
        natsInternalConn *ic = (natsInternalConn*)sub->conn->internal;
        subMap_Delete(ic->subscriptions, sub->sid);

        // 发送 UNSUB 协议
        char buf[256];
        int n = natsProto_EncodeUnsub(buf, sizeof(buf), sub->sid, 0);
        if (n > 0) natsConn_send(sub->conn, buf, n);
    }

    free(sub->subject);
    free(sub->queue);
    free(sub);
    return NATS_OK;
}

int natsSubscription_AutoUnsubscribe(natsSubscription *sub, int maxMsgs) {
    if (!sub) return NATS_ERR_INVALID_ARG;
    sub->maxMsgs = maxMsgs;
    return NATS_OK;
}

// 消息操作
const char* natsMessage_GetSubject(natsMessage *msg) {
    return msg ? msg->subject : NULL;
}

const char* natsMessage_GetReply(natsMessage *msg) {
    return msg ? msg->reply : NULL;
}

const char* natsMessage_GetData(natsMessage *msg) {
    return msg ? msg->data : NULL;
}

int natsMessage_GetDataLength(natsMessage *msg) {
    return msg ? msg->dataLen : 0;
}

int natsMessage_Reply(natsMessage *msg, const char *data, int dataLen) {
    if (!msg || !msg->reply) return NATS_ERR_INVALID_ARG;
    return natsConnection_Publish(msg->conn, msg->reply, data, dataLen);
}

int natsMessage_ReplyString(natsMessage *msg, const char *str) {
    if (!str) return NATS_ERR_INVALID_ARG;
    return natsMessage_Reply(msg, str, (int)strlen(str));
}

void natsMessage_Retain(natsMessage *msg) {
    if (msg) msg->refs++;
}

void natsMessage_Destroy(natsMessage *msg) {
    if (!msg) return;
    if (--msg->refs > 0) return;

    free(msg->subject);
    free(msg->reply);
    free(msg->data);
    // Headers
    if (msg->hdrKeys) {
        for (int i = 0; i < msg->hdrCount; i++) {
            free(msg->hdrKeys[i]);
            free(msg->hdrVals[i]);
        }
        free(msg->hdrKeys);
        free(msg->hdrVals);
    }
    free(msg);
}

// 同步订阅（简化实现，实际需要更多工作）
natsSubscription* natsConnection_SubscribeSync(natsConnection *conn, const char *subject) {
    // 同步订阅在异步基础上实现，handler 为 NULL
    return natsConnection_Subscribe(conn, subject, NULL, NULL);
}

natsMessage* natsSubscription_NextMsg(natsSubscription *sub, int64_t timeout) {
    // 简化实现 - 实际应该使用条件变量等待消息
    // 这里直接返回 NULL，表示未实现
    (void)sub;
    (void)timeout;
    return NULL;
}

// Request-Reply 相关
char* natsConnection_NewInbox(natsConnection *conn) {
    if (!conn) return NULL;
    static int inboxCounter = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "_INBOX.%p.%d", (void*)conn, inboxCounter++);
    return nats_strdup(buf);
}

natsMessage* natsConnection_Request(natsConnection *conn, const char *subject,
                                     const char *data, int dataLen, int64_t timeout) {
    (void)conn;
    (void)subject;
    (void)data;
    (void)dataLen;
    (void)timeout;
    // 简化实现 - 实际需要创建 inbox 订阅，发送消息，等待响应
    return NULL;
}

int natsConnection_RequestAsync(natsConnection *conn, const char *subject,
                                 const char *data, int dataLen,
                                 natsMsgHandler handler, void *userdata) {
    (void)conn;
    (void)subject;
    (void)data;
    (void)dataLen;
    (void)handler;
    (void)userdata;
    // 简化实现
    return NATS_ERR;
}

// Header 相关（简化实现）
const char* natsMessage_GetHeader(natsMessage *msg, const char *key) {
    if (!msg || !key) return NULL;
    for (int i = 0; i < msg->hdrCount; i++) {
        if (strcmp(msg->hdrKeys[i], key) == 0) {
            return msg->hdrVals[i];
        }
    }
    return NULL;
}

int natsMessage_GetHeaders(natsMessage *msg, char ***keys, char ***vals, int *count) {
    if (!msg) return NATS_ERR_INVALID_ARG;
    if (keys) *keys = msg->hdrKeys;
    if (vals) *vals = msg->hdrVals;
    if (count) *count = msg->hdrCount;
    return NATS_OK;
}

int natsSubscription_SetPendingLimits(natsSubscription *sub, int msgLimit, int bytesLimit) {
    (void)sub;
    (void)msgLimit;
    (void)bytesLimit;
    // 简化实现
    return NATS_OK;
}

int natsConnection_FlushTimeout(natsConnection *conn, int64_t timeout) {
    (void)timeout;
    return natsConn_flush(conn);
}

const char* natsStatus_GetText(int status) {
    switch (status) {
        case NATS_OK: return "OK";
        case NATS_ERR: return "Error";
        case NATS_ERR_INVALID_ARG: return "Invalid argument";
        case NATS_ERR_NO_MEMORY: return "No memory";
        case NATS_ERR_CONNECTION_CLOSED: return "Connection closed";
        case NATS_ERR_CONNECTION_LOST: return "Connection lost";
        case NATS_ERR_TIMEOUT: return "Timeout";
        case NATS_ERR_PROTOCOL: return "Protocol error";
        case NATS_ERR_AUTH_FAILED: return "Authentication failed";
        case NATS_ERR_MAX_PAYLOAD: return "Max payload exceeded";
        case NATS_ERR_SUBSCRIPTION_CLOSED: return "Subscription closed";
        case NATS_ERR_NO_RESPONDERS: return "No responders";
        default: return "Unknown error";
    }
}
