#ifndef __XRPC_AWAITER_H__
#define __XRPC_AWAITER_H__
#include "xpack.h"
#include "xcoroutine.h"
#include <unordered_map>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <coroutine>

// 简化后的 RpcResponseManager：只保存 coroutine handle / co_id 与结果缓存
class RpcResponseManager {
public:
    struct RpcEntry {
        std::coroutine_handle<> handle = nullptr; // 若需要兼容旧 resume，可同时保存 co_id
        int co_id = -1;
        std::unique_ptr<XPackBuff> pending_result;
        bool has_result = false;
        int error_code = 0;
    };

    static RpcResponseManager& instance() {
        static RpcResponseManager mgr;
        return mgr;
    }

    void register_rpc_marker(uint32_t pkg_id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto &e = map_[pkg_id];
        e.handle = nullptr;
        e.co_id = -1;
        e.pending_result.reset();
        e.has_result = false;
        e.error_code = 0;
    }

    // 在 await_suspend 中注册 coroutine handle（或记录 co_id）
    void register_waiter(uint32_t pkg_id, std::coroutine_handle<> h) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto &e = map_[pkg_id];
        e.handle = h;
        e.co_id = coroutine_self_id(); // 兼容需要 id 的代码路径
        // 如果已经有 pending 结果，直接 resume（协程 resume 后会在 await_resume 取走结果）
        if (e.has_result) {
            // 注意：先复制必要数据然后在锁外 resume
            auto should_resume = e.handle;
            lk.~lock_guard(); // release before resume (but we already left scope; here just logical)
            if (should_resume && should_resume != std::noop_coroutine()) {
                should_resume.resume();
            }
        }
    }

    // 网络收到响应时调用
    void complete_rpc(uint32_t pkg_id, XPackBuff&& result) {
        std::coroutine_handle<> to_resume = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = map_.find(pkg_id);
            if (it == map_.end()) {
                // 缓存响应，等待未来的 waiter
                map_[pkg_id].pending_result = std::make_unique<XPackBuff>(std::move(result));
                map_[pkg_id].has_result = true;
                return;
            }
            auto &e = it->second;
            if (e.handle) {
                to_resume = e.handle;
                // 把结果存入 pending，以便 await_resume 读取
                e.pending_result = std::make_unique<XPackBuff>(std::move(result));
                e.has_result = true;
                // 可以删除 entry 或保留直到 await_resume 完成 remove_rpc 调用
                map_.erase(it);
            } else {
                e.pending_result = std::make_unique<XPackBuff>(std::move(result));
                e.has_result = true;
            }
        }
        if (to_resume && to_resume != std::noop_coroutine()) {
            to_resume.resume();
        }
    }

    void complete_rpc_with_error(uint32_t pkg_id, int error_code) {
        std::coroutine_handle<> to_resume = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = map_.find(pkg_id);
            if (it == map_.end()) {
                auto &e = map_[pkg_id];
                e.has_result = true;
                e.error_code = error_code;
                return;
            }
            auto &e = it->second;
            if (e.handle) {
                to_resume = e.handle;
                e.has_result = true;
                e.error_code = error_code;
                map_.erase(it);
            } else {
                e.has_result = true;
                e.error_code = error_code;
            }
        }
        if (to_resume && to_resume != std::noop_coroutine()) {
            to_resume.resume();
        }
    }

    // await_resume 从这里取结果（或 error）
    std::optional<XPackBuff> take_response(uint32_t pkg_id, int *out_error = nullptr) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(pkg_id);
        if (it == map_.end()) return std::nullopt;
        if (it->second.has_result) {
            if (it->second.pending_result) {
                XPackBuff r = std::move(*it->second.pending_result);
                map_.erase(it);
                return r;
            } else {
                if (out_error) *out_error = it->second.error_code;
                map_.erase(it);
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    void remove_rpc(uint32_t pkg_id) {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.erase(pkg_id);
    }

private:
    std::mutex mtx_;
    std::unordered_map<uint32_t, RpcEntry> map_;
};

// 简化后的 awaiter：只保存 pkg_id，在 suspend 时注册 handle，resume 后从 manager 取数据
class xrpc_awaiter {
public:
    explicit xrpc_awaiter(uint32_t pkg_id = 0) : pkg_id_(pkg_id) {}
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        RpcResponseManager::instance().register_waiter(pkg_id_, h);
    }

    XPackBuff await_resume() noexcept {
        int error = 0;
        auto maybe = RpcResponseManager::instance().take_response(pkg_id_, &error);
        if (maybe) return std::move(*maybe);
        if (error != 0) return XPackBuff{ nullptr, error }; // 用 len < 0 表示错误码
        return XPackBuff{ nullptr, -1 }; // 表示未收到结果（不应发生）
    }

    void set_error(int error_code) noexcept {
        RpcResponseManager::instance().complete_rpc_with_error(pkg_id_, error_code);
    }

    uint32_t pkg_id() const noexcept { return pkg_id_; }
    void set_pkg_id(uint32_t id) noexcept { pkg_id_ = id; }

private:
    uint32_t pkg_id_;
};
#endif // __XRPC_AWAITER_H__
