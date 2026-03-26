// Copyright 2025 The xnet Authors
// Licensed under the Apache License, Version 2.0

#ifndef NATS_PROTOCOL_H
#define NATS_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// NATS 协议命令
#define NATS_OP_INFO     "INFO"
#define NATS_OP_CONNECT  "CONNECT"
#define NATS_OP_PUB      "PUB"
#define NATS_OP_HPUB     "HPUB"
#define NATS_OP_SUB      "SUB"
#define NATS_OP_UNSUB    "UNSUB"
#define NATS_OP_MSG      "MSG"
#define NATS_OP_HMSG     "HMSG"
#define NATS_OP_PING     "PING"
#define NATS_OP_PONG     "PONG"
#define NATS_OP_OK       "+OK"
#define NATS_OP_ERR      "-ERR"

// 协议常量
#define NATS_CRLF        "\r\n"
#define NATS_CRLF_LEN    2
#define NATS_MAX_CONTROL_LINE 4096
#define NATS_MAX_MSG_SIZE (64 * 1024 * 1024)  // 64MB

// 解析器状态
typedef enum {
    PARSER_START = 0,
    PARSER_OP_I,        // INFO 的第一个字母
    PARSER_OP_IN,
    PARSER_OP_INF,
    PARSER_OP_INFO,
    PARSER_OP_INFO_SPC,
    PARSER_INFO_ARG,

    PARSER_OP_M,        // MSG
    PARSER_OP_MS,
    PARSER_OP_MSG,
    PARSER_OP_MSG_SPC,
    PARSER_MSG_ARGS,
    PARSER_MSG_PAYLOAD,
    PARSER_MSG_END,

    PARSER_OP_H,        // HMSG
    PARSER_OP_HM,
    PARSER_OP_HMS,
    PARSER_OP_HMSG,
    PARSER_OP_HMSG_SPC,
    PARSER_HMSG_ARGS,
    PARSER_HMSG_HDRS,
    PARSER_HMSG_PAYLOAD,

    PARSER_OP_P,        // PUB/PING/PONG
    PARSER_OP_PI,       // PING
    PARSER_OP_PIN,
    PARSER_OP_PING,
    PARSER_OP_PO,       // PONG
    PARSER_OP_PON,
    PARSER_OP_PONG,

    PARSER_OP_PLUS,     // +OK
    PARSER_OP_PLUS_O,
    PARSER_OP_PLUS_OK,

    PARSER_OP_MINUS,    // -ERR
    PARSER_OP_MINUS_E,
    PARSER_OP_MINUS_ER,
    PARSER_OP_MINUS_ERR,
    PARSER_OP_MINUS_ERR_SPC,
    PARSER_ERR_ARG,

    PARSER_OP_S,        // SUB
    PARSER_OP_SU,
    PARSER_OP_SUB,

    PARSER_OP_U,        // UNSUB
    PARSER_OP_UN,
    PARSER_OP_UNS,
    PARSER_OP_UNSU,
    PARSER_OP_UNSUB,
} natsParseState;

// 消息参数
typedef struct {
    char    subject[256];
    char    reply[256];
    int64_t sid;
    int     hdrSize;
    int     payloadSize;
} natsMsgArgs;

// 解析器
typedef struct {
    natsParseState state;

    // 当前解析的命令参数
    natsMsgArgs args;

    // 临时缓冲区
    char    lineBuf[NATS_MAX_CONTROL_LINE];
    int     linePos;

    // 头部缓冲区
    char    *hdrBuf;
    int     hdrPos;

    // 消息体缓冲区
    char    *payloadBuf;
    int     payloadPos;

    // 解析回调
    void    *userdata;
    void    (*onInfo)(void *userdata, const char *json, int len);
    void    (*onMsg)(void *userdata, natsMsgArgs *args, const char *hdrs, int hdrLen,
                     const char *payload, int payloadLen);
    void    (*onPing)(void *userdata);
    void    (*onPong)(void *userdata);
    void    (*onError)(void *userdata, const char *err, int len);
    void    (*onOK)(void *userdata);
} natsParser;

// 解析器 API
natsParser* natsParser_Create(void *userdata);
void natsParser_Destroy(natsParser *parser);

// 设置回调
void natsParser_SetCallbacks(natsParser *parser,
    void (*onInfo)(void*, const char*, int),
    void (*onMsg)(void*, natsMsgArgs*, const char*, int, const char*, int),
    void (*onPing)(void*),
    void (*onPong)(void*),
    void (*onError)(void*, const char*, int),
    void (*onOK)(void*));

// 解析数据，返回消耗的字节数
int natsParser_Parse(natsParser *parser, const char *data, int len);

// 重置解析器状态
void natsParser_Reset(natsParser *parser);

// 协议编码
int natsProto_EncodeConnect(char *buf, int bufSize,
                            const char *name, bool verbose, bool pedantic,
                            const char *user, const char *pass, const char *token,
                            const char *jwt, const char *nkey,
                            bool headers, bool noResponders);

int natsProto_EncodePub(char *buf, int bufSize,
                        const char *subject, const char *reply,
                        const char *data, int dataLen);

int natsProto_EncodeSub(char *buf, int bufSize,
                        const char *subject, const char *queue, int64_t sid);

int natsProto_EncodeUnsub(char *buf, int bufSize, int64_t sid, int maxMsgs);

int natsProto_EncodePing(char *buf, int bufSize);
int natsProto_EncodePong(char *buf, int bufSize);

// JSON 解析辅助
int natsProto_ParseInfo(const char *json, int len,
                        char *serverId, int serverIdSize,
                        char *serverVersion, int versionSize,
                        int64_t *maxPayload,
                        bool *headersSupported);

#ifdef __cplusplus
}
#endif

#endif /* NATS_PROTOCOL_H_ */
