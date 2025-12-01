#include <iostream>
#include <thread>
#include <chrono>
#include "xcoroutine.h"

// 简单的协程函数，用于测试正常的协程执行
xCoroTask simple_coroutine(void* arg) {
    std::cout << "Simple coroutine started with arg: " << (const char*)arg << std::endl;

    // 获取当前协程ID
    int my_id = coroutine_self_id();
    std::cout << "My coroutine ID is: " << my_id << std::endl;

    // 直接运行一些代码
    std::cout << "Running initial code..." << std::endl;

    // 挂起协程
    std::vector<VariantType> result = co_await xAwaiter();
    std::cout << "Coroutine resumed with result" << std::endl;

    std::cout << "Coroutine finished, ID: " << coroutine_self_id() << std::endl;
    co_return;
}

// 会导致段错误的协程函数，用于测试异常处理
xCoroTask crash_coroutine(void* arg) {
    std::cout << "Crash coroutine started" << std::endl;

    // 故意制造段错误
    int* ptr = nullptr;
    *ptr = 42;  // 这将导致段错误

    std::cout << "This should not be printed" << std::endl;
    co_return;
}

int main() {
    std::cout << "Starting coroutine exception test..." << std::endl;

    if (!coroutine_init()) {
        std::cerr << "Failed to initialize coroutine system" << std::endl;
        return 1;
    }

    // 测试正常的协程
    std::cout << "\n=== Testing normal coroutine ===" << std::endl;
    int coro_id1 = coroutine_run(simple_coroutine, (void*)"hello");
    if (coro_id1 >= 0) {
        std::cout << "Started normal coroutine with ID: " << coro_id1 << std::endl;
        // 恢复协程执行
        coroutine_resume(coro_id1, nullptr);
    }

    // 测试会导致崩溃的协程
    std::cout << "\n=== Testing crash coroutine ===" << std::endl;
    int coro_id2 = coroutine_run(crash_coroutine, nullptr);
    if (coro_id2 >= 0) {
        std::cout << "Started crash coroutine with ID: " << coro_id2 << std::endl;
        // 恢复协程执行，这应该触发异常处理
        coroutine_resume(coro_id2, nullptr);
    }

    std::cout << "\nTest completed" << std::endl;
    coroutine_uninit();
    return 0;
}