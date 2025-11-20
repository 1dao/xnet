#include "rpc.h"
#include "zmalloc.h"
#include "xcoroutine.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// 全局RPC管理器
static RpcManager* rpc_manager = NULL;

// 初始化RPC管理器
static void init_rpc_manager() {
    rpc_manager = zmalloc(sizeof(RpcManager));
    rpc_manager->capacity = 1024;
    rpc_manager->contexts = zmalloc(sizeof(RpcContext) * rpc_manager->capacity);
    rpc_manager->count = 0;
    rpc_manager->next_pkg_id = 1000; // 起始包ID
    pthread_mutex_init(&rpc_manager->mutex, NULL);
}

// 初始化RPC模块
void rpc_init(void) {
    if (!rpc_manager) {
        init_rpc_manager();
    }
}

// 查找RPC上下文
static RpcContext* find_rpc_context(uint32_t pkg_id) {
    pthread_mutex_lock(&rpc_manager->mutex);
    for (int i = 0; i < rpc_manager->count; i++) {
        if (rpc_manager->contexts[i].pkg_id == pkg_id) {
            RpcContext* ctx = &rpc_manager->contexts[i];
            pthread_mutex_unlock(&rpc_manager->mutex);
            return ctx;
        }
    }
    pthread_mutex_unlock(&rpc_manager->mutex);
    return NULL;
}

// 移除RPC上下文
static void remove_rpc_context(uint32_t pkg_id) {
    pthread_mutex_lock(&rpc_manager->mutex);
    for (int i = 0; i < rpc_manager->count; i++) {
        if (rpc_manager->contexts[i].pkg_id == pkg_id) {
            // 移动最后一个元素覆盖当前位置
            if (i < rpc_manager->count - 1) {
                rpc_manager->contexts[i] = rpc_manager->contexts[rpc_manager->count - 1];
            }
            rpc_manager->count--;
            break;
        }
    }
    pthread_mutex_unlock(&rpc_manager->mutex);
}

// RPC超时回调
static void rpc_timeout_callback(aeEventLoop* el, long long id, void* clientData) {
    uint32_t pkg_id = *(uint32_t*)clientData;
    RpcContext* ctx = find_rpc_context(pkg_id);

    if (ctx) {
        ctx->result.success = false;
        ctx->result.timeout = true;

        // 恢复协程
        if (ctx->coroutine_handle) {
            coroutine_resume(ctx->coroutine_handle);
        }

        // 减少计数
        rpc_decr_count(ctx->channel);
        // 移除上下文
        remove_rpc_context(pkg_id);
    }

    zfree(clientData);
}

// 构建RPC请求包
static char* build_rpc_request(uint16_t protocol, uint32_t pkg_id,
    const char* data, int len, int* packet_len) {
    // 协议结构参考现有代码
    typedef struct {
        uint32_t pkg_len;
        uint16_t protocol;
        uint8_t need_return;
        uint8_t is_request;
        uint32_t pkg_id;
        int param1;
    } RpcHeader;

    *packet_len = sizeof(RpcHeader) + len;
    char* packet = zmalloc(*packet_len);
    if (!packet) return NULL;

    RpcHeader* header = (RpcHeader*)packet;
    header->pkg_len = *packet_len;
    header->protocol = protocol;
    header->need_return = 1; // RPC需要返回
    header->is_request = 1;  // 这是请求包
    header->pkg_id = pkg_id;
    header->param1 = 0;      // 预留参数

    // 复制数据
    if (len > 0 && data) {
        memcpy(packet + sizeof(RpcHeader), data, len);
    }

    return packet;
}

// 增加RPC计数并设置标记
void rpc_incr_count(aeChannel* channel) {
    if (!channel || !channel->ev) return;

    // 原子增加计数（简化实现）
    static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&count_mutex);

    // 在channel用户数据中存储RPC计数
    if (!channel->userdata) {
        channel->userdata = zmalloc(sizeof(int));
        *(int*)channel->userdata = 0;
    }

    int* count = (int*)channel->userdata;
    (*count)++;

    // 如果是第一个RPC请求，添加AE_RPC标记
    if (*count == 1 && !(channel->ev->mask & AE_RPC)) {
        aeEventLoop* el = aeGetCurEventLoop();
        aeCreateFileEvent(el, channel->fd, AE_RPC, NULL, channel, &channel->ev);
    }

    pthread_mutex_unlock(&count_mutex);
}

// 减少RPC计数并移除标记（如果需要）
void rpc_decr_count(aeChannel* channel) {
    if (!channel || !channel->userdata || !channel->ev) return;

    static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&count_mutex);

    int* count = (int*)channel->userdata;
    if (*count > 0) {
        (*count)--;

        // 如果计数为0，移除AE_RPC标记
        if (*count == 0) {
            aeEventLoop* el = aeGetCurEventLoop();
            aeDeleteFileEvent(el, channel->fd, channel->ev, AE_RPC);
        }
    }

    pthread_mutex_unlock(&count_mutex);
}

// 处理RPC响应
int rpc_on_packet(aeChannel* channel, char* buf, int len) {
    if (len < sizeof(uint32_t)) return 0;

    // 解析包长度和包ID
    uint32_t pkg_len = *(uint32_t*)buf;
    if (len < pkg_len) return 0; // 包不完整

    // 解析包ID（根据协议结构偏移）
    uint32_t pkg_id = *(uint32_t*)(buf + 8); // 参考ProtocolPacket结构

    // 查找对应的RPC上下文
    RpcContext* ctx = find_rpc_context(pkg_id);
    if (!ctx) return 0; // 不是RPC响应包，交给外部处理

    // 解析响应内容
    ctx->result.success = true;
    ctx->result.timeout = false;
    ctx->result.pkg_id = pkg_id;
    ctx->result.param1 = *(int*)(buf + 12); // 参考协议结构

    // 提取参数2
    int param2_len = pkg_len - 16; // 头部长度
    if (param2_len > 0) {
        ctx->result.param2 = zmalloc(param2_len + 1);
        memcpy(ctx->result.param2, buf + 16, param2_len);
        ctx->result.param2[param2_len] = '\0';
        ctx->result.param2_len = param2_len;
    }

    // 取消超时定时器
    aeEventLoop* el = aeGetCurEventLoop();
    aeDeleteTimeEvent(el, ctx->timer_id);

    // 恢复协程
    if (ctx->coroutine_handle) {
        coroutine_resume(ctx->coroutine_handle);
    }

    // 减少计数
    rpc_decr_count(channel);
    // 移除上下文
    remove_rpc_context(pkg_id);

    return pkg_len; // 已处理的字节数
}

// 发起RPC调用（协程方式）
RpcResult rpc_call(aeChannel* channel, uint16_t protocol, const char* data, int len, int timeout_ms) {
    if (!rpc_manager) rpc_init();
    if (!channel) {
        RpcResult res = { false, false, 0, 0, NULL, 0 };
        return res;
    }

    // 获取当前协程句柄
    void* current_coroutine = coroutine_self();
    if (!current_coroutine) {
        RpcResult res = { false, false, 0, 0, NULL, 0 };
        return res;
    }

    // 生成包ID
    uint32_t pkg_id = rpc_manager->next_pkg_id++;

    // 创建RPC上下文
    RpcContext ctx;
    memset(&ctx, 0, sizeof(RpcContext));
    ctx.pkg_id = pkg_id;
    ctx.channel = channel;
    ctx.coroutine_handle = current_coroutine;

    // 添加到管理器
    pthread_mutex_lock(&rpc_manager->mutex);
    if (rpc_manager->count >= rpc_manager->capacity) {
        // 扩容
        rpc_manager->capacity *= 2;
        rpc_manager->contexts = zrealloc(rpc_manager->contexts,
            sizeof(RpcContext) * rpc_manager->capacity);
    }
    rpc_manager->contexts[rpc_manager->count++] = ctx;
    pthread_mutex_unlock(&rpc_manager->mutex);

    // 增加RPC计数并设置标记
    rpc_incr_count(channel);

    // 构建并发送RPC请求
    int packet_len;
    char* request = build_rpc_request(protocol, pkg_id, data, len, &packet_len);
    if (!request) {
        rpc_decr_count(channel);
        remove_rpc_context(pkg_id);
        RpcResult res = { false, false, pkg_id, 0, NULL, 0 };
        return res;
    }

    // 发送请求
    int send_len = ae_channel_send(channel, request, packet_len);
    zfree(request);

    if (send_len != packet_len) {
        rpc_decr_count(channel);
        remove_rpc_context(pkg_id);
        RpcResult res = { false, false, pkg_id, 0, NULL, 0 };
        return res;
    }

    // 设置超时定时器
    aeEventLoop* el = aeGetCurEventLoop();
    uint32_t* pkg_id_ptr = zmalloc(sizeof(uint32_t));
    *pkg_id_ptr = pkg_id;
    ctx.timer_id = aeCreateTimeEvent(el, timeout_ms, rpc_timeout_callback, pkg_id_ptr, NULL);

    // 更新上下文的timer_id
    RpcContext* ctx_in_manager = find_rpc_context(pkg_id);
    if (ctx_in_manager) {
        ctx_in_manager->timer_id = ctx.timer_id;
    }

    // 挂起协程
    coroutine_yield(coroutine_self);

    // 协程恢复后，获取结果
    RpcContext* result_ctx = find_rpc_context(pkg_id);
    RpcResult res = { false, false, pkg_id, 0, NULL, 0 };

    if (result_ctx) {
        res = result_ctx->result;
    }

    return res;
}