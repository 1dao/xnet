// coroutine_manager.h
#ifndef _XCOROUTINE_H
#define _XCOROUTINE_H

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#endif
#include <chrono>
#include <coroutine>
#include <iostream>
#include <functional>
#include <memory>
#include <vector>
#include <variant>
#include <string>
#include <any>
#include <optional>
#include <unordered_map>

#include "xpack.h"

// 协程函数类型
struct xTask;
typedef xTask   (*fnCoro)(void*);

// 初始化协程管理器
bool coroutine_init();
void coroutine_uninit();

// 单参数版本
int coroutine_run(fnCoro func, void* arg);

// 通过ID恢复特定协程并传递参数
bool coroutine_resume(int coroutine_id, void* param);
bool coroutine_resume(uint32_t pkg_id, std::vector<VariantType>&& resp);

// 恢复所有可运行的协程
void coroutine_resume_all();

// 检查协程是否已完成
bool coroutine_is_done(int coroutine_id);

// 获取活跃协程数量
size_t coroutine_get_active_count();

// 获取当前协程的ID（在协程内部调用）
int coroutine_self_id();


// 自定义 Final Awaiter - 在协程结束时自动清理
struct xFinAwaiter {
    int coroutine_id;

    xFinAwaiter(int id) : coroutine_id(id) {}

    bool await_ready() noexcept {
        return false;  // 总是挂起，以便执行清理
    }

    // 返回 coroutine_handle<> 允许我们控制接下来恢复哪个协程
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept;

    void await_resume() noexcept {}
};

// 简化的协程任务类型
struct xTask {
    struct promise_type {
        int coroutine_id = 0;
        void* resume_param = nullptr;
        promise_type() = default;

        xTask get_return_object() {
            return xTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() { return {}; }

        // 使用自定义的 xFinAwaiter 实现自动清理
        xFinAwaiter final_suspend() noexcept {
            return xFinAwaiter(coroutine_id);
        }

        void unhandled_exception() {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                std::cerr << "Coroutine exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Coroutine unknown exception" << std::endl;
            }
        }
        void return_void() {}

        template<typename Awaitable>
        auto await_transform(Awaitable&& awaitable) {
            return std::forward<Awaitable>(awaitable);
        }

        template<typename T>
        auto yield_value(T&& v) {
            // coroutine_id 在 coroutine manager 启动时会被设置为当前协程ID
            return v.awaiter_cvt();
        }
    };

    std::coroutine_handle<promise_type> handle_;

    xTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
    xTask(const xTask&) = delete;
    xTask& operator=(const xTask&) = delete;
    xTask(xTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr;}
    xTask& operator=(xTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    ~xTask() { if (handle_) handle_.destroy();}

    bool done() const { return !handle_ || handle_.done();}
    void resume(void* param) {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }
    void* address() const {
        return handle_ ? handle_.address() : nullptr;
    }

    promise_type& get_promise() {
        return handle_.promise();
    }
};

// Awaiter：协程挂起/恢复的桥接
class xAwaiter {
public:
    explicit xAwaiter(uint32_t pkg_id = 0) noexcept : pkg_id_(pkg_id) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    std::vector<VariantType> await_resume() noexcept;

    uint32_t pkg_id() const noexcept { return pkg_id_; }
private:
    uint32_t pkg_id_;
};

#endif // _XCOROUTINE_H
