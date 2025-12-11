// ae_channel.h
#ifndef _AE_CHANNEL_H
#define _AE_CHANNEL_H
#include "ae.h"

#include <stdint.h>
#include <stdbool.h>

typedef enum xProto {
    xproto_blp2        = 0,  // 2字节长度字段
    xproto_blp4        = 1,  // 4字节长度字段
    xproto_crlf_resp2  = 2,  // crlf-redis-resp2协议
    xproto_crlf_resp3  = 3,  // crlf-redis-resp3协议
    xproto_max               // 协议数量
} xProto;

typedef struct xChannel {
    xSocket fd;
    int     wlen;
    char* wbuf;
    char* wpos;

    int	    rlen;
    char* rbuf;
    char* rpos;

    aeFileEvent* ev;
    xProto pproto;          // 通道协议类型
    void* userdata;         // 用户数据指针

    uint8_t  is_rpc;
    uint32_t pk_id;
    uint32_t co_id;
    uint32_t pt;
} xChannel;

typedef int xchannel_proc(struct xChannel* s, char* buf, int len);

// 函数声明
xChannel*   xchannel_conn(char* addr, int port, xchannel_proc* on_pack, xchannel_proc* on_close, void* userdata, xProto proto = xProto::xproto_blp4);
int         xchannel_listen(int port, char* bindaddr, xchannel_proc* proc, xchannel_proc* on_close, void* userdata, xProto proto = xProto::xproto_blp4);
int         xchannel_send(struct xChannel* s, const char* buf, int len);
int         xchannel_rawsend(struct xChannel* s, const char* buf, int len);
int         xchannel_close(struct xChannel* s);

#endif
