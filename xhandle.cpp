#include "xhandle.h"
#include <unordered_map>
#include <stdexcept>
#include "xlog.h"
#include "xpack.h"
#include "xrpc.h"
#include "xcoroutine.h"
#include "zmalloc.h"

/*+----------+----------+--------+---------+---------+
  | is_rpc   | wait_id  | co_id  | retcode | data... |
  | (2bytes) | (4bytes) |(4bytes)| (4bytes)|         |
  +----------+----------+--------+---------+---------+*/

// 全局 handle 存储
std::unordered_map<int, ProtocolPostHandler> _handles_post;
std::unordered_map<int, ProtocolRPCHandler> _handles_rpc;

void xhandle_reg_post(int pt, ProtocolPostHandler handler) {
    if (_handles_post.find(pt) != _handles_post.end()) {
        throw std::runtime_error("Protocol already registered");
    }
    _handles_post[pt] = handler;
}

void xhandle_reg_rpc(int pt, ProtocolRPCHandler handler) {
    if (_handles_rpc.find(pt) != _handles_rpc.end()) {
        throw std::runtime_error("Protocol already registered");
    }
    _handles_rpc[pt] = handler;
}

struct xCoroArgs {
    xChannel* channel;
    std::shared_ptr<std::vector<VariantType>> args;
    void* handler;
    int protocol;
    uint32_t wait_id;
    int co_id;

    // POST 构造函数
    xCoroArgs(xChannel* s, std::vector<VariantType>&& a, void* h, int pt)
        : channel(s),
        args(std::make_shared<std::vector<VariantType>>(std::move(a))),
        handler(h),
        protocol(pt),
        wait_id(0),
        co_id(0) {
    }

    // RPC 构造函数
    xCoroArgs(xChannel* s, std::vector<VariantType>&& a, void* h, int pt, uint32_t wid, int cid)
        : channel(s),
        args(std::make_shared<std::vector<VariantType>>(std::move(a))),
        handler(h),
        protocol(pt),
        wait_id(wid),
        co_id(cid) {
    }

    static xCoroArgs* create_post(xChannel* s, std::vector<VariantType>&& a, void* h, int pt) {
        void* mem = zmalloc(sizeof(xCoroArgs));
        if (!mem) return nullptr;
        return new (mem) xCoroArgs(s, std::move(a), h, pt);
    }

    static xCoroArgs* create_rpc(xChannel* s, std::vector<VariantType>&& a, void* h, int pt, uint32_t wid, int cid) {
        void* mem = zmalloc(sizeof(xCoroArgs));
        if (!mem) return nullptr;
        return new (mem) xCoroArgs(s, std::move(a), h, pt, wid, cid);
    }

    static void destroy(xCoroArgs* obj) {
        if (obj) {
            obj->~xCoroArgs();
            zfree(obj);
        }
    }
};

struct xCoroArgsDeleter {
    void operator()(xCoroArgs* ptr) const {
        xCoroArgs::destroy(ptr);
    }
};

// POST协议的协程处理函数
xTask coroutine_func_post(void* arg) {
    std::unique_ptr<xCoroArgs, xCoroArgsDeleter> ctx(static_cast<xCoroArgs*>(arg));

    try {
        xlog_info("xhandle Starting POST protocol %d", ctx->protocol);

        auto handler = static_cast<ProtocolPostHandler>(ctx->handler);
        int ret = handler(ctx->channel, *(ctx->args));

        if (ret < 0) {
            xlog_err("xhandle POST protocol %d handler returned error: %d", ctx->protocol, ret);
        } else {
            xlog_info("xhandle POST protocol %d completed", ctx->protocol);
        }
    } catch (const std::exception& e) {
        xlog_err("xhandle POST protocol %d exception: %s", ctx->protocol, e.what());
    } catch (...) {
        xlog_err("xhandle POST protocol %d unknown exception", ctx->protocol);
    }

    co_return;
}

// RPC协议的协程处理函数
xTask coroutine_func_rpc(void* arg) {
    std::unique_ptr<xCoroArgs, xCoroArgsDeleter> ctx(static_cast<xCoroArgs*>(arg));
    XPackBuff result;
    int retcode = XNET_SUCCESS;

    try {
        xlog_debug("xhandle Starting RPC protocol %d, wait_id: %u", ctx->protocol, ctx->wait_id);

        auto handler = static_cast<ProtocolRPCHandler>(ctx->handler);
        result = handler(ctx->channel, *(ctx->args));

        xlog_debug("xhandle RPC protocol %d completed", ctx->protocol);
    } catch (const std::exception& e) {
        xlog_err("xhandle RPC protocol %d exception: %s", ctx->protocol, e.what());
        result = xpack_pack(true, e.what());
        retcode = XNET_CORO_EXCEPT;
    } catch (...) {
        xlog_err("xhandle RPC protocol %d unknown exception", ctx->protocol);
        result = xpack_pack(true, "Unknown exception");
        retcode = XNET_CORO_EXCEPT;
    }

    // 发送RPC响应（带执行结果）
    _xrpc_resp(ctx->channel, ctx->co_id, ctx->wait_id, retcode, result);

    co_return;
}

int xhandle_on_pack(xChannel* s, char* buf, int len) {
    uint16_t is_rpc = 0;
    uint32_t wait_id = 0;
    int co_id = 0;
    uint16_t protocol = 0;

    char* cur = s->rbuf + _xchannel_header_size(s);
    is_rpc = ntohs(*(uint16_t*)cur);
    cur += sizeof(is_rpc);

    if (is_rpc == 0) {
        // POST协议处理
        protocol = ntohs(*(uint16_t*)cur);
        cur += sizeof(protocol);

        auto it = _handles_post.find(protocol);
        if (it == _handles_post.end()) {
            xlog_err("xhandle POST protocol %d not found", protocol);
            return -1;
        }

        // 计算剩余数据长度
        int header_len = sizeof(is_rpc) + sizeof(protocol);
        int data_len = len - header_len;

        std::vector<VariantType> args;
        if (data_len > 0) {
            args = xpack_unpack(cur, data_len);
        }

        xCoroArgs* ctx = xCoroArgs::create_post(s, std::move(args), (void*)it->second, protocol);
        if (!ctx) {
            xlog_err("Failed to allocate memory for POST protocol %d", protocol);
            return len;
        }

        int coro_id = coroutine_run(coroutine_func_post, ctx);
        if (coro_id < 0) {
            xlog_err("xhandle Failed to start coroutine for POST protocol %d", protocol);
            xCoroArgs::destroy(ctx);
            return len;
        }

        return len;
    } else if (is_rpc == 2) {
        // RPC响应处理
        wait_id = ntohl(*(uint32_t*)cur);
        cur += sizeof(wait_id);
        co_id = ntohl(*(int*)cur);
        cur += sizeof(co_id);

        // 解析执行结果
        int retcode = ntohl(*(int*)cur);
        cur += sizeof(retcode);

        // 计算剩余数据长度
        int header_len = sizeof(is_rpc) + sizeof(wait_id) + sizeof(co_id) + sizeof(retcode);
        int data_len = len - header_len;

        std::vector<VariantType> res;

        // 只有有剩余数据时才解包
        if (data_len > 0) {
            res = xpack_unpack(cur, data_len);
        }

        // 将 retcode 插入到结果最前面
        res.insert(res.begin(), retcode);

        // 恢复等待的协程
        coroutine_resume(wait_id, std::move(res));

        return len;
    } else if (is_rpc == 1) {
        // RPC请求处理
        wait_id = ntohl(*(uint32_t*)cur);
        cur += sizeof(wait_id);
        co_id = ntohl(*(int*)cur);
        cur += sizeof(co_id);
        protocol = ntohs(*(uint16_t*)cur);
        cur += sizeof(protocol);

        auto it = _handles_rpc.find(protocol);
        if (it == _handles_rpc.end()) {
            xlog_err("RPC protocol %d not found", protocol);

            XPackBuff empty;
            _xrpc_resp(s, co_id, wait_id, XNET_PROTO_UNKNOWN, empty);
            return len;
        }

        // 计算剩余数据长度
        int header_len = sizeof(is_rpc) + sizeof(wait_id) + sizeof(co_id) + sizeof(protocol);
        int data_len = len - header_len;

        std::vector<VariantType> args;
        if (data_len > 0) {
            args = xpack_unpack(cur, data_len);
        }

        xCoroArgs* ctx = xCoroArgs::create_rpc(s, std::move(args), (void*)it->second, protocol, wait_id, co_id);
        if (!ctx) {
            xlog_err("Failed to allocate memory for RPC protocol %d", protocol);

            XPackBuff empty;
            _xrpc_resp(s, co_id, wait_id, XNET_MEM_FAIL, empty);
            return len;
        }

        int coro_id = coroutine_run(coroutine_func_rpc, ctx);
        if (coro_id < 0) {
            xlog_err("Failed to start coroutine for RPC protocol %d", protocol);

            XPackBuff empty;
            _xrpc_resp(s, co_id, wait_id, XNET_CORO_FAILED, empty);
            xCoroArgs::destroy(ctx);
            return -1;
        }

        return len;
    } else {
        xlog_err("Unknown RPC flag: %d", is_rpc);
        return -1;
    }
}
