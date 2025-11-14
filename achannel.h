// ae_channel.h
#ifndef _AE_CHANNEL_H
#define _AE_CHANNEL_H
#include "ae.h"
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef enum aeChannelProto {
    aeproto_blp = 0,
} aeChannelProto;

typedef struct aeChannel {
    int   fd;
    int   wlen;
    char* wbuf;
    char* wpos;

    int	  rlen;
    char* rbuf;
    char* rpos;

    void*   userdata;  // 用户数据指针
    aeFileEvent* ev;
} aeChannel;

typedef int achannel_proc(struct aeChannel* s, char* buf, int len);

// 函数声明
aeChannel*  ae_channel_conn(char* addr, int port, achannel_proc* on_pack, achannel_proc* on_close, void* userdata);
int         ae_channel_listen(int port, char* bindaddr, achannel_proc* proc, achannel_proc* on_close, void* userdata);
int         ae_channel_send(struct aeChannel* s, char* buf, int len);
int         ae_channel_xrpc(struct aeChannel* s, char* buf, int len);
int         ae_channel_close(struct aeChannel* s);

#ifdef __cplusplus
}
#endif
#endif
