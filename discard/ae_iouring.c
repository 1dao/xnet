/* ae_iouring.c - Linux io_uring based event loop implementation
 * Optimized for xChannel buffer management */
#include <liburing.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include "ae.h"
#include "zmalloc.h"
#include "xchannel.h"

#define IOURING_QUEUE_DEPTH 4096

typedef struct aeApiState {
    struct io_uring ring;
    int event_count;
} aeApiState;

// IO操作类型定义
typedef enum {
    IO_OP_READ = 1,
    IO_OP_WRITE = 2,
    IO_OP_ACCEPT = 3
} io_operation_t;

// 每个IO请求的元数据 - 与xChannel完美结合
typedef struct io_request {
    channel_context_t* ctx;        // 关联的channel上下文
    io_operation_t op;             // 操作类型
    int fd;                        // 文件描述符
    size_t bytes_transferred;      // 传输的字节数
    char* buffer;                  // 缓冲区指针（指向xChannel的wbuf/rbuf）
    size_t buffer_size;            // 缓冲区大小
    size_t buffer_offset;          // 缓冲区偏移量（用于部分发送）
} io_request_t;

static int aeApiCreate(aeEventLoop* eventLoop) {
    aeApiState* state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    // 初始化io_uring
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    // 启用SQPOLL模式提升性能
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;  // SQ线程空闲2ms后退出

    int ret = io_uring_queue_init_params(IOURING_QUEUE_DEPTH, &state->ring, &params);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        zfree(state);
        return -1;
    }

    state->event_count = 0;
    eventLoop->apidata = state;

    printf("io_uring event loop initialized with %d entries\n", IOURING_QUEUE_DEPTH);
    return 0;
}

static void aeApiFree(aeEventLoop* eventLoop) {
    aeApiState* state = eventLoop->apidata;
    if (!state) return;

    io_uring_queue_exit(&state->ring);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop* eventLoop, xSocket fd, int mask, aeFileEvent* fe) {
    aeApiState* state = eventLoop->apidata;
    if (!state) return -1;

    state->event_count++;

    // 为新连接立即提交读取请求
    if (mask & AE_READABLE) {
        channel_context_t* ctx = (channel_context_t*)fe->clientData;
        if (ctx && ctx->channel) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&state->ring);
            if (sqe) {
                xChannel* s = ctx->channel;

                // 设置读取请求，直接使用xChannel的接收缓冲区
                io_uring_prep_read(sqe, fd, s->rbuf, s->rlen, 0);

                // 设置用户数据
                io_request_t* req = zmalloc(sizeof(io_request_t));
                req->ctx = ctx;
                req->op = IO_OP_READ;
                req->fd = fd;
                req->buffer = s->rbuf;
                req->buffer_size = s->rlen;
                req->buffer_offset = 0;
                req->bytes_transferred = 0;

                io_uring_sqe_set_data(sqe, req);

                // 立即提交，不等待批量
                io_uring_submit(&state->ring);

                printf("Submitted read request for new connection fd %d\n", fd);
            }
        }
    }

    return 0;
}

static void aeApiDelEvent(aeEventLoop* eventLoop, xSocket fd, int mask) {
    aeApiState* state = eventLoop->apidata;
    if (!state) return;

    state->event_count--;
    printf("Removed event mask %d for fd %d\n", mask, fd);
}

// 提交写入请求 - 使用xChannel的wbuf
static int submit_write_request(aeApiState* state, channel_context_t* ctx) {
    if (!ctx || !ctx->channel) return -1;

    xChannel* s = ctx->channel;
    int fd = s->fd;

    // 检查是否有数据要发送
    int data_len = (int)(s->wpos - s->wbuf);
    if (data_len <= 0) {
        return 0;  // 没有数据要发送
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        // SQ队列满，等待下次轮询
        return -1;
    }

    // 设置写入请求，直接使用xChannel的发送缓冲区
    io_uring_prep_write(sqe, fd, s->wbuf, data_len, 0);

    // 设置用户数据
    io_request_t* req = zmalloc(sizeof(io_request_t));
    req->ctx = ctx;
    req->op = IO_OP_WRITE;
    req->fd = fd;
    req->buffer = s->wbuf;
    req->buffer_size = data_len;
    req->buffer_offset = 0;
    req->bytes_transferred = 0;

    io_uring_sqe_set_data(sqe, req);

    printf("Submitted write request for fd %d, len %d\n", fd, data_len);
    return 0;
}

// 提交读取请求 - 使用xChannel的rbuf
static int submit_read_request(aeApiState* state, channel_context_t* ctx) {
    if (!ctx || !ctx->channel) return -1;

    xChannel* s = ctx->channel;
    int fd = s->fd;

    // 计算可用的接收缓冲区空间
    int available = s->rlen - (int)(s->rpos - s->rbuf);
    if (available <= 0) {
        // 接收缓冲区满，需要先处理数据
        return -1;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        return -1;
    }

    // 设置读取请求，使用当前rpos位置作为读取目标
    io_uring_prep_read(sqe, fd, s->rpos, available, 0);

    // 设置用户数据
    io_request_t* req = zmalloc(sizeof(io_request_t));
    req->ctx = ctx;
    req->op = IO_OP_READ;
    req->fd = fd;
    req->buffer = s->rpos;  // 指向当前写入位置
    req->buffer_size = available;
    req->buffer_offset = 0;
    req->bytes_transferred = 0;

    io_uring_sqe_set_data(sqe, req);

    return 0;
}

// 提交accept请求
static int submit_accept_request(aeApiState* state, channel_context_t* ctx) {
    if (!ctx || !ctx->channel) return -1;

    int fd = ctx->channel->fd;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        return -1;
    }

    // 设置accept请求
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    io_uring_prep_accept(sqe, fd, (struct sockaddr*)&client_addr, &client_len, 0);

    // 设置用户数据
    io_request_t* req = zmalloc(sizeof(io_request_t));
    req->ctx = ctx;
    req->op = IO_OP_ACCEPT;
    req->fd = fd;

    io_uring_sqe_set_data(sqe, req);

    return 0;
}

static int aeApiPoll(aeEventLoop* eventLoop, struct timeval* tvp) {
    aeApiState* state = eventLoop->apidata;
    if (!state || state->event_count == 0) {
        if (tvp) {
            // 如果没有事件，等待指定时间
            struct timespec ts;
            ts.tv_sec = tvp->tv_sec;
            ts.tv_nsec = tvp->tv_usec * 1000;
            io_uring_wait_cqe_timeout(&state->ring, NULL, &ts);
        }
        return 0;
    }

    int numevents = 0;

    // 首先检查所有需要发送数据的连接
    for (int i = 0; i < AE_SETSIZE && numevents < AE_SETSIZE; i++) {
        aeFileEvent* fe = &eventLoop->events[i];
        if (fe->mask == AE_NONE) continue;

        // 如果有待发送数据且可写，提交发送请求
        if (fe->mask & AE_WRITABLE) {
            channel_context_t* ctx = (channel_context_t*)fe->clientData;
            if (ctx && ctx->channel) {
                xChannel* s = ctx->channel;
                if (s->wpos > s->wbuf) {
                    // 有数据需要发送
                    submit_write_request(state, ctx);
                }
            }
        }
    }

    // 提交所有待处理的SQ请求
    int submitted = io_uring_submit(&state->ring);
    if (submitted < 0) {
        fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-submitted));
    }

    // 处理完成事件
    struct io_uring_cqe* cqe;
    unsigned head;
    int count = 0;

    // 计算超时
    unsigned wait_time = 0;
    if (tvp) {
        wait_time = tvp->tv_sec * 1000 + tvp->tv_usec / 1000;
    }

    io_uring_for_each_cqe(&state->ring, head, cqe) {
        if (numevents >= AE_SETSIZE) break;

        io_request_t* req = (io_request_t*)io_uring_cqe_get_data(cqe);
        if (!req) {
            fprintf(stderr, "No user data for CQE\n");
            continue;
        }

        int result = cqe->res;
        channel_context_t* ctx = req->ctx;

        if (result < 0) {
            // IO操作失败
            if (result != -ECONNRESET && result != -EPIPE) {
                fprintf(stderr, "IO operation failed on fd %d: %s\n",
                    req->fd, strerror(-result));
            }

            // 标记连接错误/关闭
            eventLoop->fired[numevents].fd = req->fd;
            eventLoop->fired[numevents].mask = AE_READABLE; // 用可读表示错误/关闭
            eventLoop->fired[numevents].trans = 0;
            eventLoop->fired[numevents].fe = ctx ? ctx->channel->ev : NULL;
            numevents++;
        } else {
            // IO操作成功
            req->bytes_transferred = result;

            switch (req->op) {
            case IO_OP_READ: {
                if (ctx && ctx->channel) {
                    xChannel* s = ctx->channel;

                    if (result > 0) {
                        // 更新接收缓冲区位置
                        s->rpos += result;

                        eventLoop->fired[numevents].fd = req->fd;
                        eventLoop->fired[numevents].mask = AE_READABLE;
                        eventLoop->fired[numevents].trans = result;
                        eventLoop->fired[numevents].fe = s->ev;
                        numevents++;

                        // 如果缓冲区还有空间，立即重新提交读取请求
                        int available = s->rlen - (int)(s->rpos - s->rbuf);
                        if (available > 1024) {  // 至少有1KB空间
                            submit_read_request(state, ctx);
                        }
                    }
                    else if (result == 0) {
                        // 对端关闭连接
                        eventLoop->fired[numevents].fd = req->fd;
                        eventLoop->fired[numevents].mask = AE_READABLE;
                        eventLoop->fired[numevents].trans = 0;
                        eventLoop->fired[numevents].fe = s->ev;
                        numevents++;
                    }
                }
                break;
            }

            case IO_OP_WRITE: {
                if (ctx && ctx->channel) {
                    xChannel* s = ctx->channel;

                    if (result > 0) {
                        // 成功发送了数据，调整发送缓冲区
                        if (result == (s->wpos - s->wbuf)) {
                            // 全部发送完成
                            s->wpos = s->wbuf;
                        }
                        else {
                            // 部分发送，移动剩余数据
                            memmove(s->wbuf, s->wbuf + result,
                                (s->wpos - s->wbuf) - result);
                            s->wpos = s->wbuf + ((s->wpos - s->wbuf) - result);
                        }

                        eventLoop->fired[numevents].fd = req->fd;
                        eventLoop->fired[numevents].mask = AE_WRITABLE;
                        eventLoop->fired[numevents].trans = result;
                        eventLoop->fired[numevents].fe = s->ev;
                        numevents++;

                        // 如果还有数据要发送，继续提交
                        if (s->wpos > s->wbuf) {
                            submit_write_request(state, ctx);
                        }
                    }
                }
                break;
            }

            case IO_OP_ACCEPT: {
                if (result >= 0 && ctx) {
                    // 新连接建立，result是新fd
                    eventLoop->fired[numevents].fd = req->fd;
                    eventLoop->fired[numevents].mask = AE_READABLE;
                    eventLoop->fired[numevents].trans = result;  // 新fd放在trans中
                    eventLoop->fired[numevents].fe = ctx->channel->ev;
                    numevents++;

                    // 重新提交accept请求继续接受新连接
                    submit_accept_request(state, ctx);
                }
                break;
            }

            default:
                break;
            }
        }

        // 释放请求结构
        zfree(req);
        count++;
    }

    // 标记这些CQE为已处理
    io_uring_cq_advance(&state->ring, count);

    return numevents;
}

static char* aeApiName(void) {
    return "io_uring";
}

// 专门为xChannel优化的发送函数
int aeIouringChannelSend(xChannel* s) {
    if (!s || !s->ev) return -1;

    aeEventLoop* eventLoop = aeGetCurEventLoop();
    if (!eventLoop) return -1;

    aeApiState* state = eventLoop->apidata;
    if (!state) return -1;

    channel_context_t* ctx = (channel_context_t*)s->ev->clientData;
    if (!ctx) return -1;

    return submit_write_request(state, ctx);
}

// 专门为xChannel优化的接收函数
int aeIouringChannelRecv(xChannel* s) {
    if (!s || !s->ev) return -1;

    aeEventLoop* eventLoop = aeGetCurEventLoop();
    if (!eventLoop) return -1;

    aeApiState* state = eventLoop->apidata;
    if (!state) return -1;

    channel_context_t* ctx = (channel_context_t*)s->ev->clientData;
    if (!ctx) return -1;

    return submit_read_request(state, ctx);
}
