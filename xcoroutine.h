// xcoroutine.h - Safe coroutine implementation with hardware exception protection
#ifndef _XCOROUTINE_H
#define _XCOROUTINE_H

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <csignal>
#include <csetjmp>
#endif
#include <coroutine>
#include <iostream>
#include <memory>
#include <vector>
#include <variant>
#include <string>
#include <exception>

#include "xpack.h"
#include "xlog.h"

// 协程函数类型
struct xCoroTask;
typedef xCoroTask(*fnCoro)(void*);

// 初始化/销毁协程管理器
bool coroutine_init();
void coroutine_uninit();

// 运行协程
int coroutine_run(fnCoro func, void* arg);

// 协程等待
void coroutine_sleep(int coroutine_id, int );

// 恢复协程
bool coroutine_resume(int coroutine_id, void* param);
bool coroutine_resume(uint32_t wait_id, std::vector<VariantType>&& resp);

// 其他接口
void coroutine_resume_all();
bool coroutine_is_done(int coroutine_id);
size_t coroutine_get_active_count();
int coroutine_self_id();

// Hardware exception protection structure
struct xCoroutineLJ {
    #ifdef _WIN32
        jmp_buf buf;
    #else
        sigjmp_buf buf;  // Linux下使用sigjmp_buf
    #endif
    void* env;
    int sig;
    bool in_protected_call;
};

// Final Awaiter
struct xFinAwaiter {
    int coroutine_id;
    xFinAwaiter(int id) : coroutine_id(id) {}
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// 协程任务类型
struct xCoroTask {
    struct promise_type {
        int coroutine_id = 0;
        std::exception_ptr exception_ptr = nullptr;
        int hardware_signal = 0;            // 硬件异常信号
        bool exception_handled = false;     // 标记异常是否已处理

        promise_type() = default;

        xCoroTask get_return_object() {
            return xCoroTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() { return {}; }

        xFinAwaiter final_suspend() noexcept {
            return xFinAwaiter(coroutine_id);
        }

        void unhandled_exception() {
            exception_ptr = std::current_exception();
            exception_handled = false;
        }

        void return_void() {}

        template<typename Awaitable>
        auto await_transform(Awaitable&& awaitable) {
            return std::forward<Awaitable>(awaitable);
        }

        // 检查是否有未处理的C++异常
        bool has_cpp_exception() const {
            return exception_ptr != nullptr && !exception_handled;
        }

        // 检查是否有硬件异常
        bool has_hardware_exception() const {
            return hardware_signal != 0;
        }

        // 检查是否有任何异常
        bool has_any_exception() const {
            return has_cpp_exception() || has_hardware_exception();
        }

        // 重新抛出并处理异常
        void handle_exception() {
            if (has_cpp_exception()) {
                try {
                    std::rethrow_exception(exception_ptr);
                } catch (const std::bad_variant_access& e) {
                    // 专门处理 variant 访问异常
                    xlog_err("Variant access exception in coroutine %d: %s", coroutine_id, e.what());
                } catch (const std::exception& e) {
                    xlog_err("C++ exception in coroutine %d: %s", coroutine_id, e.what());
                } catch (...) {
                    xlog_err("Unknown C++ exception in coroutine %d", coroutine_id);
                }
                exception_handled = true;  // 标记异常已处理
            }
        }

        // 获取异常信息
        std::string get_exception_message() const {
            if (has_cpp_exception()) {
                try {
                    std::rethrow_exception(exception_ptr);
                } catch (const std::exception& e) {
                    return std::string("C++ exception: ") + e.what();
                } catch (...) {
                    return "Unknown C++ exception";
                }
            } else if (has_hardware_exception()) {
                return std::string("Hardware exception: ") + get_hardware_exception_message();
            }
            return "";
        }

        // 获取硬件异常信息
        std::string get_hardware_exception_message() const {
            switch (hardware_signal) {
#ifndef _WIN32
            case SIGSEGV: return "Segmentation fault (memory access violation)";
            case SIGFPE:  return "Floating point exception (division by zero)";
            case SIGILL:  return "Illegal instruction";
            case SIGABRT: return "Abort signal";
            case SIGBUS:  return "Bus error";
            case SIGTRAP: return "Trace/breakpoint trap";
#else
            case EXCEPTION_ACCESS_VIOLATION: return "Access violation (memory access error)";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "Integer divide by zero";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "Floating point divide by zero";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "Illegal instruction";
            case EXCEPTION_STACK_OVERFLOW: return "Stack overflow";
#endif
            default:      return "Unknown hardware exception";
            }
        }
    };

    std::coroutine_handle<promise_type> handle_;

    xCoroTask() : handle_(nullptr) {}
    xCoroTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
    xCoroTask(const xCoroTask&) = delete;
    xCoroTask& operator=(const xCoroTask&) = delete;
    xCoroTask(xCoroTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    xCoroTask& operator=(xCoroTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    ~xCoroTask() { if (handle_) handle_.destroy(); }

    bool done() const {
        return !handle_ || handle_.done();
    }

    // 安全的恢复方法，支持硬件异常保护
    bool resume_safe(void* param, xCoroutineLJ* lj);

    promise_type& get_promise() { return handle_.promise(); }
    const promise_type& get_promise() const { return handle_.promise(); }

    // 检查是否有任何异常
    bool has_any_exception() const {
        return handle_ && get_promise().has_any_exception();
    }

    // 获取异常信息
    std::string get_exception_message() const {
        if (!handle_) return "";
        return get_promise().get_exception_message();
    }

    // 处理异常
    void handle_exception() {
        if (handle_) {
            get_promise().handle_exception();
        }
    }
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
