// coroutine_manager.h
#ifndef COROUTINE_MANAGER_H
#define COROUTINE_MANAGER_H

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


// 简化的协程任务类型
struct SimpleTask {
    struct promise_type {
        std::optional<void*> current_param;  // 存储当前参数
        int coroutine_id;                    // 协程ID

        promise_type() {
            coroutine_id = 0;
        }

        SimpleTask get_return_object() {
            return SimpleTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        // 初始不挂起，直接运行
        std::suspend_never initial_suspend() {
            return {};
        }

        std::suspend_always final_suspend() noexcept {
            return {};
        }

        void unhandled_exception() {
            try {
                std::rethrow_exception(std::current_exception());
            }
            catch (const std::exception& e) {
                std::cerr << "Coroutine exception: " << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "Coroutine unknown exception" << std::endl;
            }
        }

        void return_void() {}

        // 等待参数的 awaiter
        struct param_awaiter {
            promise_type& promise;

            bool await_ready() const noexcept {
                return promise.current_param.has_value();
            }

            void await_suspend(std::coroutine_handle<>) noexcept {
                // 等待参数被设置
            }

            void* await_resume() noexcept {
                if (promise.current_param.has_value()) {
                    void* result = promise.current_param.value();
                    promise.current_param.reset();
                    return result;
                }
                return nullptr;
            }
        };

        // 使用固定标识符 0 来等待参数
        auto await_transform(int) {
            return param_awaiter{ *this };
        }

        void set_param(void* param) {
            current_param = param;
        }
    };

    std::coroutine_handle<promise_type> handle_;

    SimpleTask(std::coroutine_handle<promise_type> h) : handle_(h) {}

    // 禁止拷贝，允许移动
    SimpleTask(const SimpleTask&) = delete;
    SimpleTask& operator=(const SimpleTask&) = delete;
    SimpleTask(SimpleTask&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    SimpleTask& operator=(SimpleTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~SimpleTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool done() const {
        return !handle_ || handle_.done();
    }

    void resume(void* param) {
        if (handle_ && !handle_.done()) {
            if (param) {
                handle_.promise().set_param(param);
            }
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


// 协程函数类型 - 返回 SimpleTask*
typedef void* (*CoroutineFunc)(void*);
// 变参协程函数类型
typedef void* (*CoroutineFuncNormal)(...);

// 初始化协程管理器
bool coroutine_init();

// 反初始化协程管理器
void coroutine_uninit();

// 启动协程并返回结果（包含ID和承诺对象）
int coroutine_run(CoroutineFunc func, void* arg);
// 变参版本 - 启动协程并传递多个参数
int coroutine_run_normal(CoroutineFuncNormal func, int arg_count, ...);

// 通过ID恢复特定协程并传递参数
bool coroutine_resume(int coroutine_id, void* param);

// 恢复所有可运行的协程
void coroutine_resume_all();

// 检查协程是否已完成
bool coroutine_is_done(int coroutine_id);

// 获取活跃协程数量
size_t coroutine_get_active_count();

// 获取当前协程的ID（在协程内部调用）
int coroutine_self_id();

#endif // COROUTINE_MANAGER_H