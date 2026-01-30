// xthread_demo.cpp - thread pool usage demo

#include "../xthread.h"
#include "../xlog.h"
#include <chrono>
#include <thread>
#include "../xcoroutine.h"

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
std::vector<VariantType> redis_get(xThread* ctx, std::vector<VariantType>& args) {
    (void)ctx;
    std::string key = pack_to_str(args[0]);
    xlog_info("[Redis Thread] GET %s", key.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string value = "value_for_" + key;
    std::vector<VariantType> result;
    result.emplace_back(XPackBuff(value.c_str(), (int)value.size()));
    return result;
}

// Compute task
std::vector<VariantType> compute_task(xThread* ctx, std::vector<VariantType>& args) {
    (void)ctx;
    int a = xpack_cast<int>(args[0]);
    int b = xpack_cast<int>(args[1]);
    xlog_info("[Compute Thread] %d + %d", a, b);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<VariantType> result;
    result.emplace_back(a + b);
    return result;
}

// ============================================================================
// Coroutine task - demonstrates RPC calls
// ============================================================================

xCoroTask test_coroutine(void* arg) {
    (void)arg;
    xlog_info_tag("[Coroutine]", "Started");

    // Call Redis thread to get data
    {
        auto result = co_await xthread_pcall(XTHR_REDIS, redis_get, std::string("user:1001"));
        if (xthread_ok(result)) {
            std::string value = pack_to_str(result[1]);
            xlog_info_tag("[Coroutine]", "Redis GET result: %s", value.c_str());
        } else {
            xlog_err_tag("[Coroutine]", "Redis GET failed: %d", xthread_retcode(result));
        }
    }

    // Call compute thread
    {
        auto result = co_await xthread_pcall(XTHR_COMPUTE, compute_task, 100, 200);
        if (xthread_ok(result)) {
            int sum = xpack_cast<int>(result[1]);
            xlog_info_tag("[Coroutine]", "Compute result: %d", sum);
        } else {
            xlog_err_tag("[Coroutine]", "Compute failed: %d", xthread_retcode(result));
        }
    }

    xlog_info_tag("[Coroutine]", "Finished");
    co_return;
}

// ============================================================================
// Main function
// ============================================================================

int main() {
    xlog_init(XLOG_DEBUG, true, true, nullptr);
    xlog_set_show_thread_name(true);
    coroutine_init();
    xthread_init();

    xthread_register_main(XTHR_MAIN, true, "Main");
    xthread_register(XTHR_REDIS, true, "Redis");
    xthread_register(XTHR_COMPUTE, true, "Compute");

    xlog_info("All threads started");

    // Start coroutine
    coroutine_run(test_coroutine, nullptr);

    // Main loop - process callbacks
    while (true) {
        xthread_update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    xthread_uninit();
    coroutine_uninit();
    xlog_uninit();

    return 0;
}
