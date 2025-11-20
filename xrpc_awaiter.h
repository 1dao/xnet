#ifndef __XRPC_AWAITER_H__
#define __XRPC_AWAITER_H__

#include "xpack.h"
#include "xcoroutine.h"
#include <unordered_map>
#include <mutex>
#include <memory>

// 返回 XPackBuff 的 awaiter - 完全避免拷贝
class xrpc_awaiter {
private:
    std::unique_ptr<XPackBuff> result_;
    uint32_t pkg_id_;
    bool ready_ = false;

public:
    // 默认构造函数
    xrpc_awaiter() = default;

    // 禁止拷贝
    xrpc_awaiter(const xrpc_awaiter&) = delete;
    xrpc_awaiter& operator=(const xrpc_awaiter&) = delete;

    // 移动构造函数
    xrpc_awaiter(xrpc_awaiter&& other) noexcept
        : result_(std::move(other.result_))
        , pkg_id_(other.pkg_id_)
        , ready_(other.ready_) {
        other.pkg_id_ = 0;
        other.ready_ = false;
    }

    // 移动赋值运算符
    xrpc_awaiter& operator=(xrpc_awaiter&& other) noexcept {
        if (this != &other) {
            result_ = std::move(other.result_);
            pkg_id_ = other.pkg_id_;
            ready_ = other.ready_;
            other.pkg_id_ = 0;
            other.ready_ = false;
        }
        return *this;
    }

    // awaiter 接口
    bool await_ready() const noexcept {
        return ready_;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        // 协程挂起，等待 RPC 响应
    }

    XPackBuff await_resume() noexcept {
        if (result_) {
            return std::move(*result_);
        }
        return XPackBuff{ nullptr, -1 };
    }

    // 设置结果
    void set_result(XPackBuff&& result) {
        result_ = std::make_unique<XPackBuff>(std::move(result));
        ready_ = true;
    }

    void set_error(int error_code) {
        result_ = std::make_unique<XPackBuff>(nullptr, error_code);
        ready_ = true;
    }

    // 获取包ID
    uint32_t get_pkg_id() const { return pkg_id_; }
    void set_pkg_id(uint32_t id) { pkg_id_ = id; }
};

// RPC 响应管理器
class RpcResponseManager {
private:
    std::unordered_map<uint32_t, std::unique_ptr<xrpc_awaiter>> waiting_rpcs_;
    std::mutex mutex_;

public:
    static RpcResponseManager& instance() {
        static RpcResponseManager instance;
        return instance;
    }

    // 注册 RPC - 接受 unique_ptr
    void register_rpc(uint32_t pkg_id, std::unique_ptr<xrpc_awaiter> awaiter) {
        std::lock_guard<std::mutex> lock(mutex_);
        waiting_rpcs_[pkg_id] = std::move(awaiter);
    }

    void complete_rpc(uint32_t pkg_id, XPackBuff&& result) {
        std::unique_ptr<xrpc_awaiter> awaiter;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = waiting_rpcs_.find(pkg_id);
            if (it != waiting_rpcs_.end()) {
                awaiter = std::move(it->second);
                waiting_rpcs_.erase(it);
            }
        }

        if (awaiter) {
            awaiter->set_result(std::move(result));
            coroutine_resume(pkg_id, nullptr);
        }
    }

    void complete_rpc_with_error(uint32_t pkg_id, int error_code) {
        std::unique_ptr<xrpc_awaiter> awaiter;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = waiting_rpcs_.find(pkg_id);
            if (it != waiting_rpcs_.end()) {
                awaiter = std::move(it->second);
                waiting_rpcs_.erase(it);
            }
        }

        if (awaiter) {
            awaiter->set_error(error_code);
            coroutine_resume(pkg_id, nullptr);
        }
    }

    void remove_rpc(uint32_t pkg_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        waiting_rpcs_.erase(pkg_id);
    }
};

#endif // __XRPC_AWAITER_H__