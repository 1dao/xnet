// xthread_demo.cpp - thread pool usage demo

#include "../xthread.h"
#include "../xlog.h"
#include <chrono>
#include <thread>
#include "../xcoroutine.h"
#include "../xlog.h"
// ============================================================================
// Helper: string <-> XPackBuff conversion
// ============================================================================

inline XPackBuff str_to_pack(const char* s) {
    return XPackBuff(s, (int)strlen(s));
}

inline std::string pack_to_str(VariantType& var) {
    XPackBuff buf = xpack_cast<XPackBuff>(var);
    return std::string(buf.get(), buf.len);
}

// ============================================================================
// Task functions for worker threads
// ============================================================================

// Redis operation simulation
std::vector<VariantType> redis_get(XThreadContext* ctx, std::vector<VariantType>& args) {
    // args[0] = key (XPackBuff)
    std::string key = pack_to_str(args[0]);

    xlog_info("[Redis Thread] GET %s", key.c_str());

    // Simulate Redis operation
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::string value = "value_for_" + key;

    std::vector<VariantType> result;
    result.emplace_back(XPackBuff(value.c_str(), (int)value.size()));
    return result;
}

// Compute task
std::vector<VariantType> compute_task(XThreadContext* ctx, std::vector<VariantType>& args) {
    int a = xpack_cast<int>(args[0]);
    int b = xpack_cast<int>(args[1]);

    xlog_info("[Compute Thread] %d + %d", a, b);

    // Simulate heavy computation
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<VariantType> result;
    result.emplace_back(a + b);
    return result;
}

// ============================================================================
// Coroutine task - demonstrates RPC calls
// ============================================================================

xTask test_coroutine(void* arg) {
    xlog_info("[Coroutine] Started");

    // 测试段错误
    if(true)
    {
        xlog_info("[Linux Coroutine] Testing segmentation fault...");
        int* ptr = nullptr;
        *ptr = 42;  // 这会触发SIGSEGV
        xlog_info("[Linux Coroutine] This line should not be reached");
    }

    // Test hardware exception (division by zero)
    if(false)
    {
        int a = 1;
        int b = 0;
        xlog_info("[Coroutine] Testing division by zero: %d / %d", a, b);
        int result = a / b;  // This will trigger hardware exception but be caught
        xlog_info("[Coroutine] This line should not be reached", result);
    }

    // Call Redis thread to get data
    {
        auto result = co_await xthread_pcall(XTHR_REDIS, redis_get, "user:1001");
        int a[3] = {};
        xlog_info("[Coroutine] Testing variant access...%d", a[5]);

        if (true) {
            int sum = xpack_cast<int>(result[11111]);
            xlog_info("[Coroutine] Testing variant access exception...", sum);
        }

        // try {
        //     // This should throw std::bad_variant_access and be caught by coroutine system
        //     int sum = xpack_cast<int>(result[11111]);
        //     xlog_info("[Coroutine] Sum: %d", sum);
        // } catch (const std::exception& e) {
        //     // This catch block should not be reached - the exception should be caught by coroutine system
        //     xlog_err("[Coroutine] Direct catch: %s", e.what());
        // }

        //xlog_info("[Coroutine] Sum: %d", sum);
        if (xthread_ok(result)) {
            std::string value = pack_to_str(result[1]);
            xlog_info("[Coroutine] Redis GET result: %s", value.c_str());
        } else {
            xlog_err("[Coroutine] Redis GET failed: %d", xthread_retcode(result));
        }
    }

    // Call compute thread
    {
        auto result = co_await xthread_pcall(XTHR_COMPUTE, compute_task, 100, 200);

        if (xthread_ok(result)) {
            int sum = xpack_cast<int>(result[1]);
            xlog_info("[Coroutine] Compute result: %d", sum);
        } else {
            xlog_err("[Coroutine] Compute failed: %d", xthread_retcode(result));
        }
    }

    xlog_info("[Coroutine] Finished");
    co_return;
}

// ============================================================================
// Comprehensive exception test coroutine
// ============================================================================

xTask comprehensive_exception_test(void* arg) {
    int test_case = arg ? *(int*)arg : 0;

    xlog_info("[Coroutine] Comprehensive exception test started - Test case: %d", test_case);

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
                // 启用浮点异常
                unsigned int old_fp_control = _controlfp(0, 0);
                _controlfp(0, _EM_INVALID | _EM_ZERODIVIDE | _EM_OVERFLOW);

                volatile double x = 1.0;
                volatile double y = 0.0;

                // 现在浮点除零应该触发异常
                if (y == 0.0) {
                    volatile double z = x / y;  // 这会触发EXCEPTION_FLT_DIVIDE_BY_ZERO
                    (void)z;
                }

                // 恢复浮点控制字
                _controlfp(old_fp_control, _MCW_EM);

                xlog_info("Floating point operations completed");
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

        case 5:  // 栈溢出（相对安全的测试）
            xlog_info("=== Testing potential stack overflow ===");
            {
                // 使用相对安全的栈分配大小
                char buffer[32 * 1024];  // 32KB栈分配
                memset(buffer, 0, sizeof(buffer));
                xlog_info("Stack allocation completed, buffer size: %zu", sizeof(buffer));
            }
            break;

        case 6:  // C++异常
            xlog_info("=== Testing C++ exceptions ===");
            {
                throw std::runtime_error("Test C++ exception from coroutine");
            }
            break;

        case 7:  // 标准库容器异常
            xlog_info("=== Testing STL container exceptions ===");
            {
                std::vector<int> vec;
                vec.reserve(10);
                try {
                    vec.at(100) = 42;  // 这会抛出std::out_of_range
                } catch (const std::exception& e) {
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

    xlog_info("[Coroutine] Exception test case %d completed successfully", test_case);
    co_return;
}

// ============================================================================
// Main function
// ============================================================================

int main() {
    // Initialize
    xlog_init(XLOG_DEBUG, true, true, nullptr);
    coroutine_init();
    xthread_init();

    // Register main thread
    xthread_register_main(XTHR_MAIN, "Main");

    // Register worker threads
    xthread_register(XTHR_REDIS, "Redis");
    xthread_register(XTHR_COMPUTE, "Compute");

    xlog_info("All threads started");

    // Start coroutine
    // coroutine_run(test_coroutine, nullptr);
     int cs_id = 10;
     coroutine_run(comprehensive_exception_test, &cs_id);
    int test_cases[] = {1, 2, 3, 4, 9, 10};  // 各种异常类型
    //for (int i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); i++) {
    //    coroutine_run(comprehensive_exception_test, &test_cases[i]);
    //}
    //comprehensive_exception_test(nullptr);

    // Main loop - process callbacks
    while(true) {
        xthread_update();  // Process RPC results from worker threads
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        //if (coroutine_get_active_count() == 0) {
        //    xlog_info("All coroutines finished");
        //    break;
        //}
    }

    // Cleanup
    xthread_uninit();
    coroutine_uninit();
    xlog_uninit();

    return 0;
}
