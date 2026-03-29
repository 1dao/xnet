// Copyright 2025 The xnet Authors
// Licensed under the Apache License, Version 2.0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nats_protocol.h"

natsParser* natsParser_Create(void *userdata) {
    natsParser *parser = (natsParser*)calloc(1, sizeof(natsParser));
    if (parser) {
        parser->userdata = userdata;
        parser->state = PARSER_START;
    }
    return parser;
}

void natsParser_Destroy(natsParser *parser) {
    if (parser) {
        free(parser->hdrBuf);
        free(parser->payloadBuf);
        free(parser);
    }
}

void natsParser_SetCallbacks(natsParser *parser,
    void (*onInfo)(void*, const char*, int),
    void (*onMsg)(void*, natsMsgArgs*, const char*, int, const char*, int),
    void (*onPing)(void*),
    void (*onPong)(void*),
    void (*onError)(void*, const char*, int),
    void (*onOK)(void*))
{
    if (!parser) return;
    parser->onInfo = onInfo;
    parser->onMsg = onMsg;
    parser->onPing = onPing;
    parser->onPong = onPong;
    parser->onError = onError;
    parser->onOK = onOK;
}

void natsParser_Reset(natsParser *parser) {
    if (!parser) return;
    parser->state = PARSER_START;
    parser->linePos = 0;
    parser->hdrPos = 0;
    parser->payloadPos = 0;
    memset(&parser->args, 0, sizeof(parser->args));
}

// 解析一行，返回行长度（包含CRLF），如果没找到完整行返回0
static int findLine(const char *data, int len) {
    for (int i = 0; i < len - 1; i++) {
        if (data[i] == '\r' && data[i+1] == '\n') {
            return i + 2;
        }
    }
    return 0;
}

// 解析 MSG/HMSG 参数: <subject> <sid> [reply] <size>
// 或 HMSG: <subject> <sid> [reply] <hdr-size> <total-size>
static int parseMsgArgs(const char *line, int lineLen, natsMsgArgs *args, bool hasHeaders) {
    char buf[256];
    if (lineLen >= sizeof(buf)) lineLen = sizeof(buf) - 1;
    memcpy(buf, line, lineLen);
    buf[lineLen] = '\0';

    char *p = buf;
    char *tokens[5];
    int nTokens = 0;

    // 分割token
    while (*p && nTokens < 5) {
        // 跳过空白
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        tokens[nTokens++] = p;

        // 找到下一个空白
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }

    // 检查参数数量
    int minTokens = hasHeaders ? 4 : 3;
    int maxTokens = hasHeaders ? 5 : 4;
    if (nTokens < minTokens || nTokens > maxTokens) {
        return -1;
    }

    // subject
    strncpy(args->subject, tokens[0], sizeof(args->subject) - 1);
    args->subject[sizeof(args->subject) - 1] = '\0';

    // sid
    args->sid = atoll(tokens[1]);

    if (hasHeaders) {
        // HMSG: <subject> <sid> [reply] <hdr-size> <total-size>
        if (nTokens == 5) {
            strncpy(args->reply, tokens[2], sizeof(args->reply) - 1);
            args->reply[sizeof(args->reply) - 1] = '\0';
            args->hdrSize = atoi(tokens[3]);
            args->payloadSize = atoi(tokens[4]) - args->hdrSize;
        } else {
            args->reply[0] = '\0';
            args->hdrSize = atoi(tokens[2]);
            args->payloadSize = atoi(tokens[3]) - args->hdrSize;
        }
    } else {
        // MSG: <subject> <sid> [reply] <size>
        if (nTokens == 4) {
            strncpy(args->reply, tokens[2], sizeof(args->reply) - 1);
            args->reply[sizeof(args->reply) - 1] = '\0';
            args->payloadSize = atoi(tokens[3]);
        } else {
            args->reply[0] = '\0';
            args->payloadSize = atoi(tokens[2]);
        }
        args->hdrSize = 0;
    }

    return 0;
}

int natsParser_Parse(natsParser *parser, const char *data, int len) {
    if (!parser || !data || len <= 0) return 0;

    int pos = 0;

    while (pos < len) {
        char c = data[pos];

        switch (parser->state) {
            case PARSER_START:
                switch (c) {
                    case 'I': parser->state = PARSER_OP_I; break;
                    case 'M': parser->state = PARSER_OP_M; break;
                    case 'H': parser->state = PARSER_OP_H; break;
                    case 'P': parser->state = PARSER_OP_P; break;
                    case 'S': parser->state = PARSER_OP_S; break;
                    case 'U': parser->state = PARSER_OP_U; break;
                    case '+': parser->state = PARSER_OP_PLUS; break;
                    case '-': parser->state = PARSER_OP_MINUS; break;
                    default:
                        // 未知命令，跳过直到CRLF
                        parser->state = PARSER_START;
                        break;
                }
                pos++;
                break;

            case PARSER_OP_I:
                parser->state = (c == 'N') ? PARSER_OP_IN : PARSER_START;
                pos++;
                break;

            case PARSER_OP_IN:
                parser->state = (c == 'F') ? PARSER_OP_INF : PARSER_START;
                pos++;
                break;

            case PARSER_OP_INF:
                parser->state = (c == 'O') ? PARSER_OP_INFO : PARSER_START;
                pos++;
                break;

            case PARSER_OP_INFO:
                if (c == ' ') {
                    parser->state = PARSER_OP_INFO_SPC;
                } else {
                    parser->state = PARSER_START;
                }
                pos++;
                break;

            case PARSER_OP_INFO_SPC:
            case PARSER_INFO_ARG: {
                // 读取到CRLF
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    // 缓存不完整行
                    int remaining = len - pos;
                    if (parser->linePos + remaining < NATS_MAX_CONTROL_LINE) {
                        memcpy(parser->lineBuf + parser->linePos, data + pos, remaining);
                        parser->linePos += remaining;
                    }
                    parser->state = PARSER_INFO_ARG;
                    return len;
                }

                // 组合缓存和新数据
                if (parser->linePos > 0) {
                    int copyLen = lineLen - 2;  // 不包括CRLF
                    if (parser->linePos + copyLen < NATS_MAX_CONTROL_LINE) {
                        memcpy(parser->lineBuf + parser->linePos, data + pos, copyLen);
                        parser->lineBuf[parser->linePos + copyLen] = '\0';
                        if (parser->onInfo) {
                            parser->onInfo(parser->userdata, parser->lineBuf, parser->linePos + copyLen);
                        }
                    }
                    parser->linePos = 0;
                } else {
                    if (parser->onInfo) {
                        parser->onInfo(parser->userdata, data + pos, lineLen - 2);
                    }
                }

                pos += lineLen;
                parser->state = PARSER_START;
                break;
            }

            case PARSER_OP_M:
                parser->state = (c == 'S') ? PARSER_OP_MS : PARSER_START;
                pos++;
                break;

            case PARSER_OP_MS:
                parser->state = (c == 'G') ? PARSER_OP_MSG : PARSER_START;
                pos++;
                break;

            case PARSER_OP_MSG:
                if (c == ' ') {
                    parser->state = PARSER_OP_MSG_SPC;
                } else {
                    parser->state = PARSER_START;
                }
                pos++;
                break;

            case PARSER_OP_MSG_SPC:
            case PARSER_MSG_ARGS: {
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    int remaining = len - pos;
                    if (parser->linePos + remaining < NATS_MAX_CONTROL_LINE) {
                        memcpy(parser->lineBuf + parser->linePos, data + pos, remaining);
                        parser->linePos += remaining;
                    }
                    parser->state = PARSER_MSG_ARGS;
                    return len;
                }

                // 解析参数
                char *line = parser->lineBuf;
                int lineTotalLen = parser->linePos + lineLen - 2;
                if (parser->linePos > 0) {
                    memcpy(parser->lineBuf + parser->linePos, data + pos, lineLen - 2);
                    parser->lineBuf[lineTotalLen] = '\0';
                } else {
                    line = (char*)data + pos;
                }

                if (parseMsgArgs(line, lineTotalLen, &parser->args, false) < 0) {
                    parser->state = PARSER_START;
                    parser->linePos = 0;
                    pos += lineLen;
                    break;
                }

                parser->linePos = 0;
                pos += lineLen;

                // 准备读取payload
                if (parser->args.payloadSize > 0) {
                    parser->payloadBuf = (char*)malloc(parser->args.payloadSize + 2);  // +2 for CRLF
                    parser->payloadPos = 0;
                    parser->state = PARSER_MSG_PAYLOAD;
                } else {
                    // 空消息，直接回调
                    if (parser->onMsg) {
                        parser->onMsg(parser->userdata, &parser->args, NULL, 0, "", 0);
                    }
                    parser->state = PARSER_START;
                }
                break;
            }

            case PARSER_MSG_PAYLOAD: {
                int need = parser->args.payloadSize + 2 - parser->payloadPos;  // +2 for CRLF
                int have = len - pos;
                int copy = (need < have) ? need : have;

                memcpy(parser->payloadBuf + parser->payloadPos, data + pos, copy);
                parser->payloadPos += copy;
                pos += copy;

                if (parser->payloadPos >= parser->args.payloadSize + 2) {
                    // 完整消息接收完毕
                    if (parser->onMsg) {
                        parser->onMsg(parser->userdata, &parser->args, NULL, 0,
                                      parser->payloadBuf, parser->args.payloadSize);
                    }
                    free(parser->payloadBuf);
                    parser->payloadBuf = NULL;
                    parser->state = PARSER_START;
                }
                break;
            }

            case PARSER_OP_H:
                if (c == 'M') {
                    parser->state = PARSER_OP_HM;
                } else if (c == 'P') {
                    parser->state = PARSER_OP_PI;  // HPUB -> 按 PUB 处理或添加 HPUB 处理
                } else {
                    parser->state = PARSER_START;
                }
                pos++;
                break;

            case PARSER_OP_HM:
                parser->state = (c == 'S') ? PARSER_OP_HMS : PARSER_START;
                pos++;
                break;

            case PARSER_OP_HMS:
                parser->state = (c == 'G') ? PARSER_OP_HMSG : PARSER_START;
                pos++;
                break;

            case PARSER_OP_HMSG:
                if (c == ' ') {
                    parser->state = PARSER_OP_HMSG_SPC;
                } else {
                    parser->state = PARSER_START;
                }
                pos++;
                break;

            case PARSER_OP_HMSG_SPC:
            case PARSER_HMSG_ARGS: {
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    int remaining = len - pos;
                    if (parser->linePos + remaining < NATS_MAX_CONTROL_LINE) {
                        memcpy(parser->lineBuf + parser->linePos, data + pos, remaining);
                        parser->linePos += remaining;
                    }
                    parser->state = PARSER_HMSG_ARGS;
                    return len;
                }

                char *line = parser->lineBuf;
                int lineTotalLen = parser->linePos + lineLen - 2;
                if (parser->linePos > 0) {
                    memcpy(parser->lineBuf + parser->linePos, data + pos, lineLen - 2);
                    parser->lineBuf[lineTotalLen] = '\0';
                } else {
                    line = (char*)data + pos;
                }

                if (parseMsgArgs(line, lineTotalLen, &parser->args, true) < 0) {
                    parser->state = PARSER_START;
                    parser->linePos = 0;
                    pos += lineLen;
                    break;
                }

                parser->linePos = 0;
                pos += lineLen;

                // 分配 header 和 payload 缓冲区
                if (parser->args.hdrSize > 0 || parser->args.payloadSize > 0) {
                    int totalNeed = parser->args.hdrSize + parser->args.payloadSize + 2;
                    parser->payloadBuf = (char*)malloc(totalNeed);
                    parser->payloadPos = 0;
                    parser->state = PARSER_HMSG_HDRS;
                } else {
                    if (parser->onMsg) {
                        parser->onMsg(parser->userdata, &parser->args, NULL, 0, "", 0);
                    }
                    parser->state = PARSER_START;
                }
                break;
            }

            case PARSER_HMSG_HDRS: {
                int need = parser->args.hdrSize - parser->payloadPos;
                int have = len - pos;
                int copy = (need < have) ? need : have;

                if (copy > 0) {
                    memcpy(parser->payloadBuf + parser->payloadPos, data + pos, copy);
                    parser->payloadPos += copy;
                    pos += copy;
                }

                if (parser->payloadPos >= parser->args.hdrSize) {
                    parser->state = PARSER_HMSG_PAYLOAD;
                }
                break;
            }

            case PARSER_HMSG_PAYLOAD: {
                int hdrOffset = parser->args.hdrSize;
                int need = hdrOffset + parser->args.payloadSize + 2 - parser->payloadPos;
                int have = len - pos;
                int copy = (need < have) ? need : have;

                if (copy > 0) {
                    memcpy(parser->payloadBuf + parser->payloadPos, data + pos, copy);
                    parser->payloadPos += copy;
                    pos += copy;
                }

                if (parser->payloadPos >= hdrOffset + parser->args.payloadSize + 2) {
                    if (parser->onMsg) {
                        parser->onMsg(parser->userdata, &parser->args,
                                      parser->payloadBuf, parser->args.hdrSize,
                                      parser->payloadBuf + parser->args.hdrSize,
                                      parser->args.payloadSize);
                    }
                    free(parser->payloadBuf);
                    parser->payloadBuf = NULL;
                    parser->state = PARSER_START;
                }
                break;
            }

            case PARSER_OP_P:
                if (c == 'I') {
                    parser->state = PARSER_OP_PI;
                } else if (c == 'O') {
                    parser->state = PARSER_OP_PO;
                } else {
                    parser->state = PARSER_START;
                }
                pos++;
                break;

            case PARSER_OP_PI:
                parser->state = (c == 'N') ? PARSER_OP_PIN : PARSER_START;
                pos++;
                break;

            case PARSER_OP_PIN:
                parser->state = (c == 'G') ? PARSER_OP_PING : PARSER_START;
                pos++;
                break;

            case PARSER_OP_PING: {
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    // 等待CRLF
                    return pos;
                }
                if (parser->onPing) {
                    parser->onPing(parser->userdata);
                }
                pos += lineLen;
                parser->state = PARSER_START;
                break;
            }

            case PARSER_OP_PO:
                parser->state = (c == 'N') ? PARSER_OP_PON : PARSER_START;
                pos++;
                break;

            case PARSER_OP_PON:
                parser->state = (c == 'G') ? PARSER_OP_PONG : PARSER_START;
                pos++;
                break;

            case PARSER_OP_PONG: {
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    return pos;
                }
                if (parser->onPong) {
                    parser->onPong(parser->userdata);
                }
                pos += lineLen;
                parser->state = PARSER_START;
                break;
            }

            case PARSER_OP_PLUS:
                parser->state = (c == 'O') ? PARSER_OP_PLUS_O : PARSER_START;
                pos++;
                break;

            case PARSER_OP_PLUS_O:
                parser->state = (c == 'K') ? PARSER_OP_PLUS_OK : PARSER_START;
                pos++;
                break;

            case PARSER_OP_PLUS_OK: {
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    return pos;
                }
                if (parser->onOK) {
                    parser->onOK(parser->userdata);
                }
                pos += lineLen;
                parser->state = PARSER_START;
                break;
            }

            case PARSER_OP_MINUS:
                parser->state = (c == 'E') ? PARSER_OP_MINUS_E : PARSER_START;
                pos++;
                break;

            case PARSER_OP_MINUS_E:
                parser->state = (c == 'R') ? PARSER_OP_MINUS_ER : PARSER_START;
                pos++;
                break;

            case PARSER_OP_MINUS_ER:
                parser->state = (c == 'R') ? PARSER_OP_MINUS_ERR : PARSER_START;
                pos++;
                break;

            case PARSER_OP_MINUS_ERR:
                if (c == ' ') {
                    parser->state = PARSER_OP_MINUS_ERR_SPC;
                } else {
                    parser->state = PARSER_START;
                }
                pos++;
                break;

            case PARSER_OP_MINUS_ERR_SPC:
            case PARSER_ERR_ARG: {
                int lineLen = findLine(data + pos, len - pos);
                if (lineLen == 0) {
                    int remaining = len - pos;
                    if (parser->linePos + remaining < NATS_MAX_CONTROL_LINE) {
                        memcpy(parser->lineBuf + parser->linePos, data + pos, remaining);
                        parser->linePos += remaining;
                    }
                    parser->state = PARSER_ERR_ARG;
                    return len;
                }

                if (parser->linePos > 0) {
                    int copyLen = lineLen - 2;
                    memcpy(parser->lineBuf + parser->linePos, data + pos, copyLen);
                    parser->lineBuf[parser->linePos + copyLen] = '\0';
                    if (parser->onError) {
                        parser->onError(parser->userdata, parser->lineBuf, parser->linePos + copyLen);
                    }
                    parser->linePos = 0;
                } else {
                    if (parser->onError) {
                        parser->onError(parser->userdata, data + pos, lineLen - 2);
                    }
                }

                pos += lineLen;
                parser->state = PARSER_START;
                break;
            }

            default:
                // 未知状态，重置
                parser->state = PARSER_START;
                pos++;
                break;
        }
    }

    return pos;
}

// 协议编码实现
int natsProto_EncodeConnect(char *buf, int bufSize,
                            const char *name, bool verbose, bool pedantic,
                            const char *user, const char *pass, const char *token,
                            const char *jwt, const char *nkey,
                            bool headers, bool noResponders) {
    int pos = 0;
    int n;

    n = snprintf(buf + pos, bufSize - pos, "CONNECT {\"verbose\":%s,\"pedantic\":%s,",
                  verbose ? "true" : "false",
                  pedantic ? "true" : "false");
    if (n < 0 || n >= bufSize - pos) return -1;
    pos += n;

    if (name) {
        n = snprintf(buf + pos, bufSize - pos, "\"name\":\"%s\",", name);
        if (n < 0 || n >= bufSize - pos) return -1;
        pos += n;
    }

    if (user && pass) {
        n = snprintf(buf + pos, bufSize - pos, "\"user\":\"%s\",\"pass\":\"%s\",", user, pass);
        if (n < 0 || n >= bufSize - pos) return -1;
        pos += n;
    }

    if (token) {
        n = snprintf(buf + pos, bufSize - pos, "\"auth_token\":\"%s\",", token);
        if (n < 0 || n >= bufSize - pos) return -1;
        pos += n;
    }

    if (jwt) {
        n = snprintf(buf + pos, bufSize - pos, "\"jwt\":\"%s\",", jwt);
        if (n < 0 || n >= bufSize - pos) return -1;
        pos += n;
    }

    if (nkey) {
        n = snprintf(buf + pos, bufSize - pos, "\"nkey\":\"%s\",", nkey);
        if (n < 0 || n >= bufSize - pos) return -1;
        pos += n;
    }

    n = snprintf(buf + pos, bufSize - pos,
                  "\"tls_required\":false,\"headers\":%s,\"no_responders\":%s,\"protocol\":1}\r\n",
                  headers ? "true" : "false",
                  noResponders ? "true" : "false");
    if (n < 0 || n >= bufSize - pos) return -1;
    pos += n;

    return pos;
}

int natsProto_EncodePub(char *buf, int bufSize,
                        const char *subject, const char *reply,
                        const char *data, int dataLen) {
    int pos = 0;
    int n;

    if (reply) {
        n = snprintf(buf + pos, bufSize - pos, "PUB %s %s %d\r\n",
                      subject, reply, dataLen);
    } else {
        n = snprintf(buf + pos, bufSize - pos, "PUB %s %d\r\n",
                      subject, dataLen);
    }
    if (n < 0 || n >= bufSize - pos) return -1;
    pos += n;

    if (dataLen > 0) {
        if (dataLen + 2 > bufSize - pos) return -1;
        memcpy(buf + pos, data, dataLen);
        pos += dataLen;
    }

    if (2 > bufSize - pos) return -1;
    memcpy(buf + pos, "\r\n", 2);
    pos += 2;

    return pos;
}

// HPUB 编码 - 支持 headers
// headers 格式: "Key1: Value1\\r\\nKey2: Value2\\r\\n"
int natsProto_EncodeHPub(char *buf, int bufSize,
                         const char *subject, const char *reply,
                         const char *headers, int hdrLen,
                         const char *data, int dataLen) {
    int pos = 0;
    int n;
    int totalSize = hdrLen + dataLen;

    // NATS headers 需要特定的 header 格式
    // 第一行是 "NATS/1.0\r\n"
    int natHdrLen = hdrLen > 0 ? hdrLen + 10 : 0; // 10 = "NATS/1.0\r\n"
    totalSize = natHdrLen + dataLen;

    if (reply) {
        n = snprintf(buf + pos, bufSize - pos, "HPUB %s %s %d %d\r\n",
                      subject, reply, natHdrLen, totalSize);
    } else {
        n = snprintf(buf + pos, bufSize - pos, "HPUB %s %d %d\r\n",
                      subject, natHdrLen, totalSize);
    }
    if (n < 0 || n >= bufSize - pos) return -1;
    pos += n;

    // 写入 headers
    if (natHdrLen > 0) {
        if (natHdrLen + 2 > bufSize - pos) return -1;
        memcpy(buf + pos, "NATS/1.0\r\n", 10);
        pos += 10;
        if (hdrLen > 0) {
            memcpy(buf + pos, headers, hdrLen);
            pos += hdrLen;
        }
    }

    // 写入 data
    if (dataLen > 0) {
        if (dataLen + 2 > bufSize - pos) return -1;
        memcpy(buf + pos, data, dataLen);
        pos += dataLen;
    }

    if (2 > bufSize - pos) return -1;
    memcpy(buf + pos, "\r\n", 2);
    pos += 2;

    return pos;
}

int natsProto_EncodeSub(char *buf, int bufSize,
                        const char *subject, const char *queue, int64_t sid) {
    if (queue) {
        return snprintf(buf, bufSize, "SUB %s %s %lld\r\n", subject, queue, (long long)sid);
    } else {
        return snprintf(buf, bufSize, "SUB %s %lld\r\n", subject, (long long)sid);
    }
}

int natsProto_EncodeUnsub(char *buf, int bufSize, int64_t sid, int maxMsgs) {
    if (maxMsgs > 0) {
        return snprintf(buf, bufSize, "UNSUB %lld %d\r\n", (long long)sid, maxMsgs);
    } else {
        return snprintf(buf, bufSize, "UNSUB %lld\r\n", (long long)sid);
    }
}

int natsProto_EncodePing(char *buf, int bufSize) {
    if (bufSize < 6) return -1;
    memcpy(buf, "PING\r\n", 6);
    return 6;
}

int natsProto_EncodePong(char *buf, int bufSize) {
    if (bufSize < 6) return -1;
    memcpy(buf, "PONG\r\n", 6);
    return 6;
}

// 简单的JSON解析辅助函数
static const char* json_getString(const char *json, const char *key, int *len) {
    const char *p = strstr(json, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p && (*p == '"' || *p == ':' || isspace((unsigned char)*p))) p++;
    if (*p != '"') return NULL;
    p++;
    const char *end = p;
    while (*end && *end != '"') {
        if (*end == '\\' && *(end+1)) end += 2;
        else end++;
    }
    *len = end - p;
    return p;
}

static bool json_getBool(const char *json, const char *key, bool defaultVal) {
    const char *p = strstr(json, key);
    if (!p) return defaultVal;
    p += strlen(key);
    while (*p && (*p == '"' || *p == ':' || isspace((unsigned char)*p))) p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return defaultVal;
}

static int64_t json_getInt64(const char *json, const char *key, int64_t defaultVal) {
    const char *p = strstr(json, key);
    if (!p) return defaultVal;
    p += strlen(key);
    while (*p && (*p == '"' || *p == ':' || isspace((unsigned char)*p))) p++;
    return atoll(p);
}

int natsProto_ParseInfo(const char *json, int len,
                        char *serverId, int serverIdSize,
                        char *serverVersion, int versionSize,
                        int64_t *maxPayload,
                        bool *headersSupported) {
    char *buf = (char*)malloc(len + 1);
    if (!buf) return -1;
    memcpy(buf, json, len);
    buf[len] = '\0';

    int valLen;
    const char *val;

    val = json_getString(buf, "server_id", &valLen);
    if (val && serverId) {
        int copyLen = valLen < serverIdSize - 1 ? valLen : serverIdSize - 1;
        memcpy(serverId, val, copyLen);
        serverId[copyLen] = '\0';
    }

    val = json_getString(buf, "version", &valLen);
    if (val && serverVersion) {
        int copyLen = valLen < versionSize - 1 ? valLen : versionSize - 1;
        memcpy(serverVersion, val, copyLen);
        serverVersion[copyLen] = '\0';
    }

    if (maxPayload) {
        *maxPayload = json_getInt64(buf, "max_payload", 1048576);  // 默认1MB
    }

    if (headersSupported) {
        *headersSupported = json_getBool(buf, "headers", false);
    }

    free(buf);
    return 0;
}
