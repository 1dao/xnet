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
#include <functional>
#include <memory>
#include <vector>
#include <variant>
#include <string>
#include <any>

// 简化的协程任务类型
struct SimpleTask {
    struct promise_type {
        int coroutine_id;

        promise_type() : coroutine_id(0) {}

        SimpleTask get_return_object() {
            return SimpleTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

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

        // 支持任何可等待类型
        template<typename Awaitable>
        auto await_transform(Awaitable&& awaitable) {
            return std::forward<Awaitable>(awaitable);
        }
    };

    std::coroutine_handle<promise_type> handle_;

    SimpleTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
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

// 协程函数类型
typedef void* (*CoroutineFunc)(void*);

// 使用 std::variant 的多类型参数支持
using CoroutineArg = std::variant<
    int,
    long,
    float,
    double,
    bool,
    const char*,
    std::string,
    void*
>;

struct VariantCoroutineArgs {
    std::vector<CoroutineArg> args;

    VariantCoroutineArgs(std::initializer_list<CoroutineArg> init_args) : args(init_args) {}

    // 安全的参数获取方法
    template<typename T>
    std::optional<T> get_arg(size_t index) const {
        if (index >= args.size()) {
            return std::nullopt;
        }

        try {
            if (std::holds_alternative<T>(args[index])) {
                return std::get<T>(args[index]);
            }
        }
        catch (const std::bad_variant_access&) {
            // 类型不匹配
        }

        return std::nullopt;
    }

    // 强制获取参数（可能抛出异常）
    template<typename T>
    T get_arg_unsafe(size_t index) const {
        return std::get<T>(args[index]);
    }

    // 获取参数数量
    size_t size() const {
        return args.size();
    }

    // 打印所有参数（用于调试）
    void print_args() const {
        std::cout << "Arguments (" << args.size() << "): ";
        for (size_t i = 0; i < args.size(); ++i) {
            std::visit([](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, int>) {
                    std::cout << "int:" << arg << " ";
                }
                else if constexpr (std::is_same_v<T, long>) {
                    std::cout << "long:" << arg << " ";
                }
                else if constexpr (std::is_same_v<T, float>) {
                    std::cout << "float:" << arg << " ";
                }
                else if constexpr (std::is_same_v<T, double>) {
                    std::cout << "double:" << arg << " ";
                }
                else if constexpr (std::is_same_v<T, bool>) {
                    std::cout << "bool:" << (arg ? "true" : "false") << " ";
                }
                else if constexpr (std::is_same_v<T, const char*>) {
                    std::cout << "string:\"" << arg << "\" ";
                }
                else if constexpr (std::is_same_v<T, std::string>) {
                    std::cout << "string:\"" << arg << "\" ";
                }
                else if constexpr (std::is_same_v<T, void*>) {
                    std::cout << "pointer:" << arg << " ";
                }
                }, args[i]);
        }
        std::cout << std::endl;
    }
};

// 初始化协程管理器
bool coroutine_init();
void coroutine_uninit();

// 单参数版本
int coroutine_run(CoroutineFunc func, void* arg);

// 使用 std::variant 的多参数版本
int coroutine_run_variant(CoroutineFunc func, const VariantCoroutineArgs& args);
int coroutine_run_variant(CoroutineFunc func, std::initializer_list<CoroutineArg> args);

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