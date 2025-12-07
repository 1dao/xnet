#include <iostream>
#include <thread>
#include <chrono>
#include "../xcoroutine.h"

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
    int test_case = arg ? *(int*)arg : 0;

    xlog_info_tag("[Coroutine]", "Comprehensive exception test started - Test case: %d", test_case);

    switch (test_case) {
    case 1:  // 内存访问异常
        xlog_info("=== Testing memory access violation ===");
        {
            volatile int* ptr = nullptr;
            *ptr = 42;  // 这会触发EXCEPTION_ACCESS_VIOLATION
        }
        break;

    case 2:  // 整数除零
        xlog_info("=== Testing integer division by zero ===");
        {
            volatile int a = 1;
            volatile int b = 0;
            // 使用条件避免编译时检测
            if (b == 0) {
                volatile int result = a / b;  // 这会触发EXCEPTION_INT_DIVIDE_BY_ZERO
                (void)result;  // 避免未使用变量警告
            }
        }
        break;

    case 3:  // 浮点异常
        xlog_info("=== Testing floating point exceptions ===");
        {
            // // 启用浮点异常
            // unsigned int old_fp_control = _controlfp(0, 0);
            // _controlfp(0, _EM_INVALID | _EM_ZERODIVIDE | _EM_OVERFLOW);

            // volatile double x = 1.0;
            // volatile double y = 0.0;

            // // 现在浮点除零应该触发异常
            // if (y == 0.0) {
            //     volatile double z = x / y;  // 这会触发EXCEPTION_FLT_DIVIDE_BY_ZERO
            //     (void)z;
            // }

            // // 恢复浮点控制字
            // _controlfp(old_fp_control, _MCW_EM);

            // xlog_info("Floating point operations completed");
        }
        break;

    case 4:  // 数组越界
        xlog_info("=== Testing array bounds violation ===");
        {
            // 使用确定会触发访问违例的方法
            volatile int* invalid_ptr = reinterpret_cast<volatile int*>(0xFFFFFFFFFFFFFFFF);
            *invalid_ptr = 42;  // 这会触发EXCEPTION_ACCESS_VIOLATION
            (void)invalid_ptr;
            xlog_info("Array access completed");
        }
        break;

    case 5:  // 栈溢出（相对安全的测试）--协程不能使用alloca
        xlog_info("=== Testing stack overflow ===");
        {
#if !defined(__APPLE__)
            // 只在非macOS平台测试栈溢出
            const size_t stack_breaker = 64 * 1024 * 1024;  // 64MB - 远超任何合理栈大小
#else
            // macOS上使用更安全的替代测试
            xlog_info("Stack overflow test disabled on macOS");
            xlog_info("(macOS has stricter stack protection)");

            // 替代测试：安全的栈使用
            const size_t stack_breaker = 1 * 1024 * 1024;  // 64MB - 远超任何合理栈大小
#endif
            // 直接尝试分配远超栈大小的数组
            char stack_killer[stack_breaker];

            // 强制使用这个数组
            for (size_t i = 0; i < stack_breaker; i += 4096) {
                stack_killer[i] = i & 0xFF;
            }

            volatile int result = 0;
            for (size_t i = 0; i < stack_breaker; i += 1024 * 1024) {
                result += stack_killer[i];
            }
            (void)result;

            xlog_info("Stack allocation completed: %zu MB", stack_breaker / (1024 * 1024));
        }
        break;
    case 6:  // C++异常
        xlog_info("=== Testing C++ exceptions ===");
        {
            // C++异常是通过throw抛出的，不是硬件异常
            // 但我们仍然可以测试协程的C++异常处理能力
            xlog_info("Throwing C++ exception...");
            throw std::runtime_error("Test C++ exception from coroutine");
            xlog_info("This line should not be reached");
        }
        break;

    case 7:  // 标准库容器异常
        xlog_info("=== Testing STL container exceptions ===");
        {
            std::vector<int> vec;
            vec.reserve(10);
            try {
                vec.at(100) = 42;  // 这会抛出std::out_of_range
            }
            catch (const std::exception& e) {
                xlog_info("STL exception caught: %s", e.what());
                // 重新抛出以测试协程异常处理
                throw;
            }
        }
        break;

    case 8:  // 堆损坏测试（谨慎使用）
        xlog_info("=== Testing heap corruption ===");
        {
            int* ptr = new int[10];
            ptr[15] = 42;  // 堆缓冲区溢出
            delete[] ptr;
            xlog_info("Heap corruption test completed");
        }
        break;

    case 9:  // 平台相关的异常测试
        xlog_info("=== Testing platform-specific exception ===");
        {
            // 使用无效内存访问，在所有平台上都有效
            volatile int* invalid_ptr = reinterpret_cast<volatile int*>(0x1);
            *invalid_ptr = 42;  // 触发访问违例
        }
        break;

    case 10: // 空指针函数调用
        xlog_info("=== Testing null pointer function call ===");
        {
            void (*func_ptr)() = nullptr;
            func_ptr();  // 调用空函数指针
        }
        break;

    default:
        xlog_info("=== No specific test case selected ===");
        xlog_info("Available test cases:");
        xlog_info("  1 - Memory access violation");
        xlog_info("  2 - Integer division by zero");
        xlog_info("  3 - Floating point exceptions");
        xlog_info("  4 - Array bounds violation");
        xlog_info("  5 - Stack overflow (safe)");
        xlog_info("  6 - C++ exceptions");
        xlog_info("  7 - STL container exceptions");
        xlog_info("  8 - Heap corruption");
        xlog_info("  9 - Platform-specific exception");
        xlog_info("  10 - Null pointer function call");
        break;
    }

    xlog_info_tag("[Coroutine]", "Exception test case %d completed successfully", test_case);
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
    int test_cases[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };  // 各种异常类型
    for (int i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        coroutine_run(crash_coroutine, &test_cases[i]);
    }
    xlog_debug("xcoroutine count:%d", coroutine_get_active_count());

    while (true) {
        //xthread_update();  // Process RPC results from worker threads
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\nTest completed" << std::endl;
    coroutine_uninit();
    return 0;
}
