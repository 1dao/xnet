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
    if(false)
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
        
        if (false) {
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
    coroutine_run(test_coroutine, nullptr);

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
