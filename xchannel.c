// ae_channel.c
#include "ae.h"
#include "anet.h"
#include "zmalloc.h"
#include <string.h>

#ifdef AE_USING_IOCP
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#endif
#include <stdio.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "xchannel.h"
#include "anet.h"

#define CHANNEL_BUFF_MAX (2*1024*1024)

typedef struct {
#ifdef   AE_USING_IOCP
    OVERLAPPED rop;
    int rmask;
    OVERLAPPED wop;
    int wmask;
#endif
    xChannel*      channel;
    xchannel_proc*  fpack;          // 协议处理器
    xchannel_proc*  fclose;          // 协议处理器
    void*           userdata;

#ifdef AE_USING_IOCP
    SOCKET new_fd;         // 用于accept操作
    WSABUF wsrbuf;         // 用于WSARecv
    WSABUF wswbuf;         // 用于WSASend
#endif
} channel_context_t;

static xChannel* create_channel(xSocket fd, void* userdata) {
    xChannel* channel = zmalloc(sizeof(xChannel));
    if (!channel) return NULL;

    channel->fd = fd;
    channel->rbuf = zmalloc(CHANNEL_BUFF_MAX);
    channel->wbuf = zmalloc(CHANNEL_BUFF_MAX);
    channel->rpos = channel->rbuf;
    channel->wpos = channel->wbuf;
    channel->rlen = CHANNEL_BUFF_MAX;
    channel->wlen = CHANNEL_BUFF_MAX;
    channel->userdata = userdata;

    return channel;
}

static void free_channel(xChannel* channel) {
    if (!channel) return;

    if (channel->fd != -1) {
        anetCloseSocket(channel->fd);
        channel->fd = -1;
    }

    if (channel->rbuf) {
        zfree(channel->rbuf);
        channel->rbuf = NULL;
    }

    if (channel->wbuf) {
        zfree(channel->wbuf);
        channel->wbuf = NULL;
    }

    zfree(channel);
}

static channel_context_t* create_context(xSocket fd, xchannel_proc* fpack, xchannel_proc* fclose, void* userdata) {
    channel_context_t* ctx = zmalloc(sizeof(channel_context_t));
    if (!ctx) return NULL;
    ctx->channel = create_channel(fd, userdata);
    ctx->fpack   = fpack;
    ctx->fclose  = fclose;
    ctx->userdata = userdata;

    if (!ctx->channel) {
        zfree(ctx);
        return NULL;
    }

#ifdef AE_USING_IOCP
    xChannel* s = ctx->channel;
    memset(&ctx->rop, 0, sizeof(OVERLAPPED));
    memset(&ctx->wop, 0, sizeof(OVERLAPPED));
    ctx->new_fd = INVALID_SOCKET;
    ctx->wsrbuf.buf = s->rbuf;
    ctx->wsrbuf.len = 0;
    ctx->wswbuf.buf = s->wbuf;
    ctx->wswbuf.len = 0;
    ctx->rmask = AE_READABLE;
    ctx->wmask = AE_WRITABLE;
#endif

    return ctx;
}

static void free_channel_context(channel_context_t* ctx) {
    if (!ctx) return;
    if (ctx->channel) {
        free_channel(ctx->channel);
        ctx->channel = NULL;
    }

#ifdef AE_USING_IOCP
    if (ctx->new_fd != INVALID_SOCKET) {
        closesocket(ctx->new_fd);
        ctx->new_fd = INVALID_SOCKET;
    }
#endif

    zfree(ctx);
}

static int on_data(channel_context_t* ctx) {
    if (!ctx || !ctx->channel) return AE_ERR;

    xChannel* s = ctx->channel;
    for (int i = 0; i < 10; i++) {
        if (!ctx->fpack) {
            if (s->rpos > s->rbuf) {
                int len = (int)(s->rpos - s->rbuf);
                xchannel_send(s, s->rbuf, len);
                s->rpos = s->rbuf;
            }
            break;
        }

        int processed = ctx->fpack(s, s->rbuf, (int)(s->rpos - s->rbuf));
        if (processed > 0) {
            int remaining = (int)(s->rpos - s->rbuf) - processed;
            if (remaining > 0) {
                memmove(s->rbuf, s->rbuf + processed, remaining);
            } else {
                remaining = 0;
            }
            s->rpos = s->rbuf + remaining;
        } else if (processed < 0) {
            return AE_ERR;
        } else {
            break;
        }

        if (s->rpos == s->rbuf) {
            break;
        }
    }
    return AE_OK;
}

#ifdef AE_USING_IOCP
static LPFN_ACCEPTEX lpAcceptEx = NULL;
inline static int initializeAcceptEx(SOCKET listenSocket) {
    if (lpAcceptEx != NULL) return 0;

    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD bytesReturned = 0;

    if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx, sizeof(guidAcceptEx),
        &lpAcceptEx, sizeof(lpAcceptEx),
        &bytesReturned, NULL, NULL) == SOCKET_ERROR) {
        return -1;
    }

    return 0;
}

inline static int aePostIocpAccept(xSocket socket, OVERLAPPED* overlapped) {
    channel_context_t* ctx = container_of(overlapped, channel_context_t, rop);
    xSocket acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (acceptSocket == INVALID_SOCKET)
        return -1;
    ctx->new_fd = acceptSocket;
    initializeAcceptEx(socket);

    DWORD bytesReceived = 0;
    xChannel* s = ctx->channel;
    if (lpAcceptEx(socket, acceptSocket, s->rbuf, 0,
        sizeof(struct sockaddr_in) + 16,
        sizeof(struct sockaddr_in) + 16,
        &bytesReceived, overlapped) == FALSE) {
        int error = WSAGetLastError();
        if (error != ERROR_IO_PENDING) {
            closesocket(acceptSocket);
            ctx->new_fd = INVALID_SOCKET;
            return -1;
        }
    }

    return 0;
}

inline static int aePostIocpRead(xSocket socket, OVERLAPPED* overlapped) {
    channel_context_t* ctx = container_of(overlapped, channel_context_t, rop);
    xChannel* s = ctx->channel;

    DWORD bytesReceived = 0;
    DWORD flags = 0;
    ctx->wsrbuf.buf = s->rpos;
    ctx->wsrbuf.len = (ULONG)(s->rlen - (s->rpos - s->rbuf));

    if (WSARecv(socket, &ctx->wsrbuf, 1, &bytesReceived, &flags, overlapped, NULL) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            return -1;
        }
    }
    return 0;
}

inline static int aePostIocpWrite(xSocket socket, OVERLAPPED* overlapped) {
    channel_context_t* ctx = container_of(overlapped, channel_context_t, wop);
    DWORD bytesSent = 0;
    DWORD dwFlags = 0;
    xChannel* s = ctx->channel;
    ctx->wswbuf.buf = s->wbuf;
    ctx->wswbuf.len = (ULONG)(s->wpos - s->wbuf);

    if (WSASend(socket
        , &ctx->wswbuf
        , 1             // bufCount
        , &bytesSent    // prepare send bytes
        , dwFlags       // flags
        , overlapped    // overlapped
        , NULL) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            return -1;
        }
    }
    return 0;
}
#endif

int aeProcRead(struct aeEventLoop* eventLoop, void* client_data, int mask, int trans) {
    channel_context_t* ctx = (channel_context_t*)client_data;
    if (!ctx || !ctx->channel) return AE_ERR;

    xChannel* s = ctx->channel;
    xSocket fd = s->fd;
#ifndef AE_USING_IOCP
    int available = s->rlen - (s->rpos - s->rbuf);
    if (available <= 0) {
        xchannel_close(s);
        return AE_ERR;
    }

    trans = anetRead(fd, s->rpos, available);
    if (trans <= 0) {
        if (trans == 0) {
            printf("Connection closed by peer, fd: %d\n", fd);
        }
        else {
            printf("Read error on fd: %d\n", fd);
        }
        xchannel_close(s);
        return AE_ERR;
    }
    s->rpos += trans;
#else
    if (trans > 0) 
        s->rpos += trans;
#endif
    if (on_data(ctx) == AE_ERR || trans==0) {
        xchannel_close(s);
        return AE_ERR;
    }
#ifdef AE_USING_IOCP
    aePostIocpRead(fd, &ctx->rop);
#endif
    return AE_OK;
}

int aeProcWrite(struct aeEventLoop* eventLoop, xSocket fd, void* client_data, int mask, int trans) {
    channel_context_t* ctx = (channel_context_t*)client_data;
    if (!ctx || !ctx->channel) return AE_ERR;

    xChannel* s = ctx->channel;
    fd = s->fd;
    int slen = (int)(s->wpos - s->wbuf);
    if (slen <= 0) {
        s->wpos = s->wbuf;
        return AE_OK;
    }
#ifndef AE_USING_IOCP
    trans = anetWrite(fd, s->wbuf, slen);
    if (trans <= 0) {
        if (trans == 0) {
            printf("Connection closed during write, fd: %d\n", fd);
        } else {
            printf("Write error on fd: %d\n", fd);
        }
        xchannel_close(s);
        return AE_ERR;
    }

    if (slen == trans) {
        s->wpos = s->wbuf;
    } else {
        memmove(s->wbuf, s->wbuf + trans, slen - trans);
        s->wpos = s->wbuf + slen - trans;
    }
#else
    if (slen == trans) {
        s->wpos = s->wbuf;
    } else {
        memmove(s->wbuf, s->wbuf + trans, slen - trans);
        s->wpos = s->wbuf + slen - trans;
    }
    if (s->wpos != s->wbuf) {
        if (aePostIocpWrite(fd, &ctx->wop) == AE_ERR) {
            xchannel_close(s);
            return AE_ERR;
        }
    } else {
        aeDeleteFileEvent(eventLoop, fd, s->ev, AE_WRITABLE);
    }
#endif

    return AE_OK;
}

int aeProcEvent(struct aeEventLoop* eventLoop, xSocket fd, void* client_data, int mask, int trans) {
    if (mask & AE_READABLE) {
        return aeProcRead(eventLoop, client_data, mask, trans);
    } else if (mask & AE_WRITABLE) {
        return aeProcWrite(eventLoop, fd, client_data, mask, trans);
    } else {
        return AE_ERR;
    }
}

int aeProcAccept(struct aeEventLoop* eventLoop, xSocket fd, void* client_data, int mask, int trans) {
    channel_context_t* cur = (channel_context_t*)client_data;
    if (!cur) return AE_ERR;
	fd = cur->channel->fd;
#ifdef AE_USING_IOCP
    if (cur->new_fd != INVALID_SOCKET) {
        SOCKET new_fd = cur->new_fd;
        anetNonBlock(NULL, new_fd);     // 设置非阻塞
        anetTcpNoDelay(NULL, new_fd);   // 禁用Nagle算法

        channel_context_t* client_ctx = create_context(
            (int)new_fd, cur->fpack, cur->fclose, cur->userdata);
        if (!client_ctx) {
            closesocket(new_fd);
            return AE_ERR;
        }

		aeFileEvent* fe = NULL;
        if (aeCreateFileEvent(eventLoop, (int)new_fd, AE_READABLE|AE_WRITABLE, aeProcEvent, client_ctx, &fe) == AE_ERR) {
            printf("Failed to create read event for new connection\n");
            free_channel_context(client_ctx);
            anetCloseSocket(new_fd);
            return AE_ERR;
        }

        client_ctx->channel->ev = fe;
        aeDeleteFileEvent(eventLoop, fd, fe, AE_WRITABLE); // register & not start
        aePostIocpRead((int)new_fd, &client_ctx->rop);

        cur->new_fd = INVALID_SOCKET;
        aePostIocpAccept(fd, &cur->rop);

        printf("New connection accepted, fd: %d\n", (int)new_fd);
        return AE_OK;
    }
#else
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    xSocket cfd = anetTcpAccept(NULL, fd, (struct sockaddr*)&sa, &salen);

    if (cfd == ANET_ERR) {
        printf("Accept error on fd: %d\n", fd);
        return AE_ERR;
    }
    printf("New connection accepted, fd: %d\n", cfd);

    anetNonBlock(NULL, cfd);
    anetTcpNoDelay(NULL, cfd);

    channel_context_t* client_ctx = create_context(cfd, cur->fpack, cur->fclose, cur->userdata);
    if (!client_ctx) {
        anetCloseSocket(cfd);
        return AE_ERR;
    }

    aeFileEvent* client_fe = NULL;
    if (aeCreateFileEvent(eventLoop, cfd, AE_READABLE | AE_WRITABLE, aeProcEvent, client_ctx, &client_fe) == AE_ERR) {
        printf("Failed to create read event for new connection, fd: %d\n", cfd);
        free_channel_context(client_ctx);
        anetCloseSocket(cfd);
        return AE_ERR;
    }

    client_ctx->channel->ev = client_fe;
#endif

    return AE_OK;
}

int xchannel_listen(int port, char* bindaddr, xchannel_proc* fpack, xchannel_proc* fclose, void* userdata) {
    aeEventLoop* el = aeGetCurEventLoop();
    if (!el) {
        printf("No event loop available\n");
        return AE_ERR;
    }
    if (!fpack) {
        printf("fpack Invalid callback\n");
        return AE_ERR;
    }
    if (!fclose) {
        printf("fclose Invalid callback\n");
        return AE_ERR;
    }

    char err[ANET_ERR_LEN];
    xSocket fd = anetTcpServer(err, port, bindaddr);
    if (fd == ANET_ERR) {
        printf("Create TCP server error: %s\n", err);
        return AE_ERR;
    }
    printf("Listening on %s:%d, fd: %d\n", bindaddr ? bindaddr : "0.0.0.0", port, (int)fd);

    channel_context_t* listen_ctx = create_context(fd, fpack, fclose, userdata);
    if (!listen_ctx) {
        anetCloseSocket(fd);
        return AE_ERR;
    }

    aeFileEvent* fe = NULL;
    if (aeCreateFileEvent(el, fd, AE_READABLE, aeProcAccept, listen_ctx, &fe) == AE_ERR) {
        printf("Failed to create accept event, fd: %d\n", (int)fd);
        free_channel_context(listen_ctx);
        anetCloseSocket(fd);
        return AE_ERR;
    }
    listen_ctx->channel->ev = fe;

#ifdef AE_USING_IOCP
    aePostIocpAccept(fd, &listen_ctx->rop);
#endif
    return AE_OK;
}

xChannel* xchannel_conn(char* addr, int port, xchannel_proc* fpack, xchannel_proc* fclose, void* userdata) {
    aeEventLoop* el = aeGetCurEventLoop();
    if (!el) {
        printf("No event loop available\n");
        return NULL;
    }
    if (!fpack) {
        printf("fpack Invalid callback\n");
        return NULL;
    }
    if (!fclose) {
        printf("fclose Invalid callback\n");
        return NULL;
    }

    char err[ANET_ERR_LEN];
    xSocket fd = anetTcpConnect(err, addr, port);
    if (fd == ANET_ERR) {
        printf("Connect to %s:%d error: %s\n", addr, port, err);
        return NULL;
    }

    if (anetTcpNoDelay(err, fd) != ANET_OK) {
        printf("Set TCP_NODELAY error: %s\n", err);
        anetCloseSocket(fd);
        return NULL;
    }
    anetNonBlock(NULL, fd);
    printf("Connected to %s:%d, fd: %d\n", addr, port, (int)fd);

    channel_context_t* client_ctx = create_context(fd, fpack, fclose, userdata);
    if (!client_ctx) {
        anetCloseSocket(fd);
        return NULL;
    }

    aeFileEvent* client_fe = NULL;
    if (aeCreateFileEvent(el, fd, AE_READABLE | AE_WRITABLE, aeProcEvent, client_ctx, &client_fe) == AE_ERR) {
        printf("Failed to create read event for connection, fd: %d\n", (int)fd);
        free_channel_context(client_ctx);
        anetCloseSocket(fd);
        return NULL;
    }

    client_ctx->channel->ev = client_fe;
    aeDeleteFileEvent(el, fd, client_fe, AE_WRITABLE);  // register & not start

#ifdef AE_USING_IOCP
    aePostIocpRead(fd, &client_ctx->rop);
#endif
    return client_ctx->channel;
}

int xchannel_send(xChannel* s, char* buf, int len) {
    if (!s || !s->wbuf) return 0;

    int remain = s->wlen - (int)(s->wpos - s->wbuf);
    if (remain < len) {
        printf("Send buffer full, fd: %d\n", (int)s->fd);
        return 0;
    }

#ifndef AE_USING_IOCP
    int slen = len;
    if (s->wpos != s->wbuf) {
        memcpy(s->wpos, buf, len);
        s->wpos += len;
        buf  = s->wbuf;
        slen = s->wpos - s->wbuf;
    }
    int trans = anetWrite(s->fd, buf, slen);
    if (trans <= 0) {
        if (trans == 0) {
            printf("Connection closed during write, fd: %d\n", s->fd);
        } else {
            printf("Write error on fd: %d\n", s->fd);
        }
        xchannel_close(s);
        return AE_ERR;
    }

    if (slen == trans) {
        s->wpos = s->wbuf;
    } else if(buf==s->wbuf) {
        memmove(s->wbuf, s->wbuf + trans, slen - trans);
        s->wpos = s->wbuf + slen - trans;
    } else {
        memcpy(s->wbuf, buf + trans, slen - trans);
        s->wpos = slen - trans;
    }
#else
    aeFileEvent* ev = s->ev;
    aeEventLoop* el = aeGetCurEventLoop();
    channel_context_t* ctx = (channel_context_t*)ev->clientData;
    memcpy(s->wpos, buf, len);
    s->wpos += len;
    if (el && (s->wpos > s->wbuf) && (!(ev->mask & AE_WRITABLE))) {
        ev->mask |= AE_WRITABLE;
        if (aePostIocpWrite(s->fd, &ctx->wop) == AE_ERR) {
            xchannel_close(s);
            return AE_ERR;
        }
    }
#endif
    return len;
}

int xchannel_rpc(struct xChannel* s, char* buf, int len) {
    return xchannel_send(s, buf, len);
}

int xchannel_close(struct xChannel* s) {
    if (!s) return AE_ERR;
    printf("Closing channel, fd: %d\n", (int)s->fd);

    aeFileEvent* ev = s->ev;
    aeEventLoop* el = aeGetCurEventLoop();
    if (el && ev && (AE_READABLE&ev->mask)) {
        channel_context_t* ctx = (channel_context_t*)ev->clientData;
        aeDeleteFileEvent(el, s->fd, ev, AE_READABLE);
        aeDeleteFileEvent(el, s->fd, ev, AE_WRITABLE);
        ctx->fclose(s, NULL, 0);

        if (ctx) {
            anetCloseSocket(s->fd);
            free_channel_context(ctx);
        }
    } else {
        free_channel(s);
    }
    return AE_OK;
}
