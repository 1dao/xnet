// xcoroutine.h
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
#include <coroutine>
#include <iostream>
#include <memory>
#include <vector>
#include <variant>
#include <string>

#include "xpack.h"

// 协程函数类型
struct xTask;
typedef xTask (*fnCoro)(void*);

// 初始化/销毁协程管理器
bool coroutine_init();
void coroutine_uninit();

// 运行协程
int coroutine_run(fnCoro func, void* arg);

// 恢复协程
bool coroutine_resume(int coroutine_id, void* param);
bool coroutine_resume(uint32_t wait_id, std::vector<VariantType>&& resp);

// 其他接口
void coroutine_resume_all();
bool coroutine_is_done(int coroutine_id);
size_t coroutine_get_active_count();
int coroutine_self_id();

// Final Awaiter
struct xFinAwaiter {
    int coroutine_id;
    xFinAwaiter(int id) : coroutine_id(id) {}
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// 协程任务类型
struct xTask {
    struct promise_type {
        int coroutine_id = 0;
        promise_type() = default;

        xTask get_return_object() {
            return xTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() { return {}; }

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
    };

    std::coroutine_handle<promise_type> handle_;

    xTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
    xTask(const xTask&) = delete;
    xTask& operator=(const xTask&) = delete;
    xTask(xTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    xTask& operator=(xTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    ~xTask() { if (handle_) handle_.destroy(); }

    bool done() const { return !handle_ || handle_.done(); }
    void resume(void* param) {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }
    promise_type& get_promise() { return handle_.promise(); }
};

// Awaiter：协程挂起/恢复的桥接
class xAwaiter {
public:
    xAwaiter() noexcept;
    explicit xAwaiter(int err) noexcept;

    bool await_ready() const noexcept { return error_code_ != 0; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    std::vector<VariantType> await_resume() noexcept;

    uint32_t wait_id() const noexcept { return wait_id_; }
    int error_code() const noexcept { return error_code_; }

private:
    uint32_t wait_id_;
    int error_code_;
    int coro_id_;
};

#endif // _XCOROUTINE_H
