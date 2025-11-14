// ae_channel.h
#ifndef _AE_CHANNEL_H
#define _AE_CHANNEL_H
#include "ae.h"
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef enum xChannelProto {
    aeproto_blp = 0,
} xChannelProto;

typedef struct xChannel {
    xSocket fd;
    int     wlen;
    char*   wbuf;
    char*   wpos;

    int	    rlen;
    char*   rbuf;
    char*   rpos;

    void*   userdata;  // 用户数据指针
    aeFileEvent* ev;
} xChannel;

typedef int xchannel_proc(struct xChannel* s, char* buf, int len);

// 函数声明
xChannel*   xchannel_conn(char* addr, int port, xchannel_proc* on_pack, xchannel_proc* on_close, void* userdata);
int         xchannel_listen(int port, char* bindaddr, xchannel_proc* proc, xchannel_proc* on_close, void* userdata);
int         xchannel_send(struct xChannel* s, char* buf, int len);
int         xchannel_rpc(struct xChannel* s, char* buf, int len);
int         xchannel_close(struct xChannel* s);

#ifdef __cplusplus
}
#endif
#endif
