#include "xhandle.h"
#include <unordered_map>
#include <stdexcept>
#include "xlog.h"
#include "xpack.h"
#include "xrpc.h"
#include "xcoroutine.h"
#include "zmalloc.h"

//定义一个全局变量，存储handle
std::unordered_map<int, ProtocolPostHandler> _handles_post;
std::unordered_map<int, ProtocolRPCHandler> _handles_rpc;

void xhandle_reg_post(int pt, ProtocolPostHandler handler) {
    // 判断是否已经注册过
    if (_handles_post.find(pt) != _handles_post.end()) {
        // 如果已经注册过，抛出异常
        throw std::runtime_error("Protocol already registered");
    }

    _handles_post[pt] = handler;
}

void xhandle_reg_rpc(int pt, ProtocolRPCHandler handler) {
    if (_handles_rpc.find(pt) != _handles_rpc.end()) {
        // 如果已经注册过，抛出异常
        throw std::runtime_error("Protocol already registered");
    }
    _handles_rpc[pt] = handler;
}

int xhandle_invoke_post(struct xChannel* s, int pt, std::vector<VariantType>& args) {
    auto it = _handles_post.find(pt);
    if (it == _handles_post.end()) {
        // 如果没有找到对应的handle，抛出异常
        throw std::runtime_error("Protocol not found");
    }

    // 调用handle
    return it->second(s, (args));
}

XPackBuff&& xhandle_invoke_rpc(struct xChannel* s, int pt, std::vector<VariantType>& args){
    auto it = _handles_rpc.find(pt);
    if (it == _handles_rpc.end()) {
        // 如果没有找到对应的handle，抛出异常
        throw std::runtime_error("Protocol not found");
    }

    // 调用handle
    return it->second(s, (args));
}

struct xCoroArgs {
    xChannel* channel;
    std::shared_ptr<std::vector<VariantType>> args;  // 使用智能指针
    void* handler;
    int protocol;
    uint32_t pkg_id;
    int co_id;

    // POST 构造函数（不需要 pkg_id 和 co_id）
    xCoroArgs(xChannel* s, std::vector<VariantType>&& a, void* h, int pt)
        : channel(s),
        args(std::make_shared<std::vector<VariantType>>(std::move(a))),
        handler(h),
        protocol(pt),
        pkg_id(0),
        co_id(0) {
    }

    // RPC 构造函数（需要 pkg_id 和 co_id）
    xCoroArgs(xChannel* s, std::vector<VariantType>&& a, void* h, int pt, uint32_t pkg, int co)
        : channel(s),
        args(std::make_shared<std::vector<VariantType>>(std::move(a))),
        handler(h),
        protocol(pt),
        pkg_id(pkg),
        co_id(co) {
    }

    // 使用 zmalloc 创建对象
    static xCoroArgs* create_post(xChannel* s, std::vector<VariantType>&& a, void* h, int pt) {
        void* mem = zmalloc(sizeof(xCoroArgs));
        if (!mem) {
            return nullptr;
        }
        // 使用 placement new 在分配的内存上构造对象
        return new (mem) xCoroArgs(s, std::move(a), h, pt);
    }

    static xCoroArgs* create_rpc(xChannel* s, std::vector<VariantType>&& a, void* h, int pt, uint32_t pkg, int co) {
        void* mem = zmalloc(sizeof(xCoroArgs));
        if (!mem) {
            return nullptr;
        }
        // 使用 placement new 在分配的内存上构造对象
        return new (mem) xCoroArgs(s, std::move(a), h, pt, pkg, co);
    }

    // 销毁对象并释放内存
    static void destroy(xCoroArgs* obj) {
        if (obj) {
            obj->~xCoroArgs();  // 显式调用析构函数
            zfree(obj);         // 释放内存
        }
    }
};

// 自定义删除器，用于 unique_ptr
struct xCoroArgsDeleter {
    void operator()(xCoroArgs* ptr) const {
        xCoroArgs::destroy(ptr);
    }
};

// POST协议的协程处理函数
// POST协议通常用于单向消息，不需要响应
// handler在协程上下文中同步调用，这样handler内部可以使用 co_await 发起RPC调用
xTask coroutine_func_post(void* arg) {
    // 使用自定义删除器的 unique_ptr，确保使用 zmalloc/zfree
    std::unique_ptr<xCoroArgs, xCoroArgsDeleter> ctx(static_cast<xCoroArgs*>(arg));

    try {
        xlog_info("Starting POST protocol %d in coroutine", ctx->protocol);

        // 调用用户的POST handler（同步调用，但在协程上下文中）
        // 这样handler内部可以使用 co_await 发起RPC调用
        auto handler = static_cast<ProtocolPostHandler>(ctx->handler);
        int ret = handler(ctx->channel, *(ctx->args));

        if (ret < 0) {
            xlog_err("POST protocol %d handler returned error: %d", ctx->protocol, ret);
        } else {
            xlog_info("POST protocol %d completed successfully", ctx->protocol);
        }
    }
    catch (const std::exception& e) {
        xlog_err("POST protocol %d exception: %s", ctx->protocol, e.what());
    }
    catch (...) {
        xlog_err("POST protocol %d unknown exception", ctx->protocol);
    }

    // unique_ptr 自动调用自定义删除器释放 ctx
    co_return;
}

// RPC协议的协程处理函数
// RPC协议需要返回响应，handler在协程上下文中同步调用
// handler内部可以使用 co_await 发起其他RPC调用
xTask coroutine_func_rpc(void* arg) {
    // 使用自定义删除器的 unique_ptr，确保使用 zmalloc/zfree
    std::unique_ptr<xCoroArgs, xCoroArgsDeleter> ctx(static_cast<xCoroArgs*>(arg));
    XPackBuff result;
    bool has_error = false;

    try {
        xlog_info("Starting RPC protocol %d, pkg_id: %u, co_id: %d",
                  ctx->protocol, ctx->pkg_id, ctx->co_id);

        // 调用用户的RPC handler（同步调用，但在协程上下文中）
        // 这样handler内部可以使用 co_await 发起其他RPC调用
        auto handler = static_cast<ProtocolRPCHandler>(ctx->handler);
        result = handler(ctx->channel, *(ctx->args));

        xlog_info("RPC protocol %d completed, pkg_id: %u", ctx->protocol, ctx->pkg_id);
    }
    catch (const std::exception& e) {
        xlog_err("RPC protocol %d exception: %s", ctx->protocol, e.what());
        // 打包错误响应：[错误码, 错误信息]
        result = xpack_pack(true, -1, e.what());
        has_error = true;
    }
    catch (...) {
        xlog_err("RPC protocol %d unknown exception", ctx->protocol);
        // 打包未知错误响应
        result = xpack_pack(true, -1, "Unknown exception");
        has_error = true;
    }

    // 发送RPC响应
    int send_ret = xrpc_resp(ctx->channel, ctx->co_id, ctx->pkg_id, result);
    if (send_ret != 0) {
        xlog_err("Failed to send RPC response for protocol %d, pkg_id: %u, error: %d",
                 ctx->protocol, ctx->pkg_id, send_ret);
    }

    // unique_ptr 自动调用自定义删除器释放 ctx
    co_return;
}

int xhandle_on_pack(xChannel* s, char* buf, int len) {
    // 解析包头
    uint16_t is_rpc = 1;
    uint32_t pkg_id = 0;
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
            xlog_err("POST protocol %d not found", protocol);
            return -1;
        }

        // 解包参数
        std::vector<VariantType> args = xpack_unpack(cur, len);

        // 使用 zmalloc 创建协程上下文
        xCoroArgs* ctx = xCoroArgs::create_post(s, std::move(args), (void*)it->second, protocol);
        if (!ctx) {
            xlog_err("Failed to allocate memory for POST protocol %d", protocol);
            return -1;
        }

        // 启动协程
        int coro_id = coroutine_run(coroutine_func_post, ctx);
        if (coro_id < 0) {
            xlog_err("Failed to start coroutine for POST protocol %d", protocol);
            xCoroArgs::destroy(ctx);
            return -1;
        }

        xlog_debug("Started coroutine %d for POST protocol %d", coro_id, protocol);
        return len;
    }
    else if (is_rpc == 2) {
        // RPC响应处理
        pkg_id = ntohl(*(uint32_t*)cur);
        cur += sizeof(pkg_id);
        co_id = ntohl(*(int*)cur);
        cur += sizeof(co_id);

        // 解包响应数据
        std::vector<VariantType> res = xpack_unpack(cur, len);

        // 恢复等待的协程
        bool resumed = coroutine_resume(pkg_id, std::move(res));
        if (!resumed) {
            xlog_err("Failed to resume coroutine for pkg_id: %u", pkg_id);
            return -1;
        }

        xlog_debug("Resumed coroutine for RPC response, pkg_id: %u, co_id: %d", pkg_id, co_id);
        return len;
    }
    else if (is_rpc == 1) {
        // RPC请求处理
        pkg_id = ntohl(*(uint32_t*)cur);
        cur += sizeof(pkg_id);
        co_id = ntohl(*(int*)cur);
        cur += sizeof(co_id);
        protocol = ntohs(*(uint16_t*)cur);
        cur += sizeof(protocol);

        auto it = _handles_rpc.find(protocol);
        if (it == _handles_rpc.end()) {
            xlog_err("RPC protocol %d not found", protocol);

            // 发送错误响应
            XPackBuff error_resp = xpack_pack(true, -1, "Protocol not found");
            xrpc_resp(s, co_id, pkg_id, error_resp);
            return len;
        }

        // 解包参数
        std::vector<VariantType> args = xpack_unpack(cur, len);

        // 使用 zmalloc 创建协程上下文
        xCoroArgs* ctx = xCoroArgs::create_rpc(s, std::move(args), (void*)it->second, protocol, pkg_id, co_id);
        if (!ctx) {
            xlog_err("Failed to allocate memory for RPC protocol %d", protocol);

            // 发送错误响应
            XPackBuff error_resp = xpack_pack(true, -1, "Memory allocation failed");
            xrpc_resp(s, co_id, pkg_id, error_resp);
            return -1;
        }

        // 启动协程
        int coro_id = coroutine_run(coroutine_func_rpc, ctx);
        if (coro_id < 0) {
            xlog_err("Failed to start coroutine for RPC protocol %d", protocol);

            // 发送错误响应
            XPackBuff error_resp = xpack_pack(true, -1, "Failed to start coroutine");
            xrpc_resp(s, co_id, pkg_id, error_resp);
            xCoroArgs::destroy(ctx);
            return -1;
        }

        xlog_debug("Started coroutine %d for RPC protocol %d, pkg_id: %u, co_id: %d",
                   coro_id, protocol, pkg_id, co_id);
        return len;
    }
    else {
        xlog_err("Unknown RPC flag: %d", is_rpc);
        return -1;
    }
}
