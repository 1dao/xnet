// xthread_demo.cpp - thread pool usage demo with ae event loop

#include "ae.h"
#include "xchannel.h"
#include "xcoroutine.h"
#include "xrpc.h"
#include "xthread.h"
#include "anet.h"
#include "xtimer.h"

#include <chrono>
#include <thread>

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

// Redis set operation
std::vector<VariantType> redis_set(xThread* ctx, std::vector<VariantType>& args) {
    std::string key = pack_to_str(args[0]);
    std::string value = pack_to_str(args[1]);

    xlog_info("[Redis Thread] SET %s = %s", key.c_str(), value.c_str());

    // Simulate Redis operation
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::vector<VariantType> result;
    result.emplace_back(XPackBuff("OK", 2));
    return result;
}

// Compute task
std::vector<VariantType> compute_task(xThread* ctx, std::vector<VariantType>& args) {
    int a = xpack_cast<int>(args[0]);
    int b = xpack_cast<int>(args[1]);

    xlog_info("[Compute Thread] %d + %d", a, b);

    // Simulate heavy computation
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<VariantType> result;
    result.emplace_back(a + b);
    return result;
}

// Heavy compute task
std::vector<VariantType> heavy_compute(xThread* ctx, std::vector<VariantType>& args) {
    int n = xpack_cast<int>(args[0]);

    xlog_info("[Compute Thread] Heavy computation: factorial of %d", n);

    // Simulate heavy computation
    long long result = 1;
    for (int i = 1; i <= n; i++) {
        result *= i;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::vector<VariantType> ret;
    ret.emplace_back((int)result);
    return ret;
}

static int _loop_flag = AE_ALL_EVENTS;

// ============================================================================
// Thread callbacks for ae event loop
// ============================================================================

// Redis thread initialization
void redis_thread_on_init(xThread* ctx) {
    xlog_info("[Redis Thread] Initializing ae event loop");

    // Create ae event loop for this thread
    aeEventLoop* el = aeCreateEventLoop(200);
    if (!el) {
        xlog_err("[Redis Thread] Failed to create ae event loop");
        return;
    }

    // Store event loop in userdata
    ctx->userdata = el;

    // Get signal fd for notification and set it in thread context
    xSocket fd = (xSocket)-1;
    aeCreateSignalFile(el);
    aeGetSignalFile(el, &fd);
    xthread_set_notify((void*)(intptr_t)fd);
    xtimer_init(100);

    xlog_info("[Redis Thread] ae event loop initialized, signal fd: %d", fd);
}

// Redis thread update (ae event loop)
void redis_thread_on_update(xThread* ctx) {
    aeEventLoop* el = (aeEventLoop*)ctx->userdata;
    if (el) {
        // Process events with short timeout to allow thread to exit
        aeProcessEvents(el, _loop_flag);
    }
}

// Redis thread cleanup
void redis_thread_on_cleanup(xThread* ctx) {
    aeEventLoop* el = (aeEventLoop*)ctx->userdata;
    if (el) {
        aeDeleteEventLoop(el);
        ctx->userdata = nullptr;
    }
    xtimer_uninit();
    xlog_info("[Redis Thread] Cleanup completed");
}

// Compute thread initialization
void compute_thread_on_init(xThread* ctx) {
    xlog_info("[Compute Thread] Initializing ae event loop");

    // Create ae event loop for this thread
    aeEventLoop* el = aeCreateEventLoop(50);
    if (!el) {
        xlog_err("[Compute Thread] Failed to create ae event loop");
        return;
    }

    // Store event loop in userdata
    ctx->userdata = el;

    // Get signal fd for notification and set it in thread context
    xSocket fd = -1;
    aeCreateSignalFile(el);
    aeGetSignalFile(el, &fd);
    xthread_set_notify((void*)(intptr_t)fd);
    xtimer_init(100);

    xlog_info("[Compute Thread] ae event loop initialized, signal fd: %d", fd);
}

// Compute thread update (ae event loop)
void compute_thread_on_update(xThread* ctx) {
    aeEventLoop* el = (aeEventLoop*)ctx->userdata;
    if (el) {
        // Process events with short timeout to allow thread to exit
        aeProcessEvents(el, _loop_flag);
    }
}

// Compute thread cleanup
void compute_thread_on_cleanup(xThread* ctx) {
    aeEventLoop* el = (aeEventLoop*)ctx->userdata;
    if (el) {
        aeDeleteEventLoop(el);
        ctx->userdata = nullptr;
    }
    xtimer_uninit();
    xlog_info("[Compute Thread] Cleanup completed");
}

// ============================================================================
// Enhanced coroutine task with built-in signal notification
// ============================================================================

xCoroTask test_coroutine_with_ae(void* arg) {
    xlog_info_tag("[Coroutine]", "Started with ae event loop support");
    // Test Redis SET operation
    {
        auto result = co_await xthread_pcall(XTHR_REDIS, redis_set, "user:1001", "John Doe");
        if (xthread_ok(result)) {
            std::string status = pack_to_str(result[1]);
            xlog_info_tag("[Coroutine]", "Redis SET result: %s", status.c_str());
            // 不需要手动调用 notify，xthread_post 内部会自动发送信号
        } else {
            xlog_err_tag("[Coroutine]", "Redis SET failed: %d", xthread_retcode(result));
        }
    }
    xlog_err("corount sleep start:");
    co_await coroutine_sleep(10000);
    xlog_err("corount sleep finish:");

    // Call Redis thread to get data
    {
        auto result = co_await xthread_pcall(XTHR_REDIS, redis_get, "user:1001");
        if (xthread_ok(result)) {
            std::string value = pack_to_str(result[1]);
            xlog_info_tag("[Coroutine]", "Redis GET result: %s", value.c_str());
            // 不需要手动调用 notify，xthread_post 内部会自动发送信号
        } else {
            xlog_err_tag("[Coroutine]", "Redis GET failed: %d", xthread_retcode(result));
        }
    }

    // Call compute thread for simple calculation
    {
        auto result = co_await xthread_pcall(XTHR_COMPUTE, compute_task, 100, 200);
        if (xthread_ok(result)) {
            int sum = xpack_cast<int>(result[1]);
            xlog_info_tag("[Coroutine]", "Compute result: %d", sum);
            // 不需要手动调用 notify，xthread_post 内部会自动发送信号
        } else {
            xlog_err_tag("[Coroutine]", "Compute failed: %d", xthread_retcode(result));
        }
    }

    // Test multiple concurrent operations
    xlog_info_tag("[Coroutine]", "Testing concurrent operations...");

    // Launch multiple operations concurrently - 会自动发送信号通知
    auto redis_task1 = xthread_pcall(XTHR_REDIS, redis_get, "config:timeout");
    auto redis_task2 = xthread_pcall(XTHR_REDIS, redis_get, "config:retry");
    auto compute_task1 = xthread_pcall(XTHR_COMPUTE, compute_task, 50, 75);
    auto compute_task2 = xthread_pcall(XTHR_COMPUTE, compute_task, 200, 300);

    // Wait for all results
    auto result1 = co_await redis_task1;
    auto result2 = co_await redis_task2;
    auto result3 = co_await compute_task1;
    auto result4 = co_await compute_task2;

    // Process results
    if (xthread_ok(result1)) {
        std::string value = pack_to_str(result1[1]);
        xlog_info_tag("[Coroutine]", "Concurrent Redis GET 1: %s", value.c_str());
    }
    if (xthread_ok(result2)) {
        std::string value = pack_to_str(result2[1]);
        xlog_info_tag("[Coroutine]", "Concurrent Redis GET 2: %s", value.c_str());
    }
    if (xthread_ok(result3)) {
        int sum = xpack_cast<int>(result3[1]);
        xlog_info_tag("[Coroutine]", "Concurrent Compute 1: %d", sum);
    }
    if (xthread_ok(result4)) {
        int sum = xpack_cast<int>(result4[1]);
        xlog_info_tag("[Coroutine]", "Concurrent Compute 2: %d", sum);
    }

    {
        auto result = co_await xthread_pcall(XTHR_WORKER_GRP1, compute_task, 200, 300);
        if (xthread_ok(result)) {
            std::string status = pack_to_str(result[1]);
            int sum = xpack_cast<int>(result4[1]);
            xlog_info_tag("[xThread]", "Group Thread Compute 2: %d", sum);
        } else {
            xlog_err_tag("[xThread]", "Group Thread Compute 2 failed: %d", xthread_retcode(result));
        }
    }

    xlog_info_tag("[Coroutine]", "All operations completed with built-in signal notification");
    co_return;
}

xCoroTask performance_test(void* arg) {
    xlog_info_tag("[Performance Test]", "Starting performance test with 100 operations");

    int success_count = 0;
    int total_operations = 100;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < total_operations; i++) {
        // Mix Redis and Compute operations
        if (i % 3 == 0) {
            // Redis operation
            char key[64];
            snprintf(key, sizeof(key), "test_key_%d", i);
            auto result = co_await xthread_pcall(XTHR_REDIS, redis_get, key);
            if (xthread_ok(result)) {
                success_count++;
            }
        } else {
            // Compute operation
            auto result = co_await xthread_pcall(XTHR_COMPUTE, compute_task, i, i * 2);
            if (xthread_ok(result)) {
                success_count++;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    xlog_info_tag("[Performance Test]", "Completed %d/%d operations in %lld ms",
                  success_count, total_operations, duration.count());
    xlog_info_tag("[Performance Test]", "Average time per operation: %.2f ms",
                  duration.count() / (double)total_operations);
    co_return;
}


// ============================================================================
// Main function with built-in signal notification
// ============================================================================

int main() {
    // Initialize
    aeEventLoop* el = aeCreateEventLoop(1024);
    if (!el) {
        xlog_err("Failed to create event loop");
        return -1;
    }
    xlog_init(XLOG_DEBUG, true, true, "logs/xlog.log");
    xlog_set_show_thread_name(true);
    coroutine_init();
    xthread_init();
    xtimer_init(500);

    // Register main thread (no ae event loop for main thread)
    xthread_register_main(XTHR_MAIN, true, "Main");

    // Register worker threads with ae event loop support
    xthread_register(XTHR_REDIS, true, "Redis",
                     redis_thread_on_init,
                     redis_thread_on_update,
                     redis_thread_on_cleanup);

    xthread_register(XTHR_COMPUTE, true, "Compute",
                     compute_thread_on_init,
                     compute_thread_on_update,
                     compute_thread_on_cleanup);
    // create & attach signal file
    {
        xSocket fd = (xSocket)-1;
        aeCreateSignalFile(el);
        aeGetSignalFile(el, &fd);
        xthread_set_notify((void*)(intptr_t)fd);
    }
    xlog_info("All threads started with built-in signal notification");
    {
        // 注册线程组（4个IO线程，使用最少队列策略）
        xthread_register_group(XTHR_WORKER_GRP1, 4, XTHSTRATEGY_LEAST_QUEUE, true, "IO_Worker",
            [](xThread* ctx) {
                aeEventLoop* el = aeCreateEventLoop(50);
                // Store event loop in userdata
                ctx->userdata = el;

                // Get signal fd for notification and set it in thread context
                xSocket fd = -1;
                aeCreateSignalFile(el);
                aeGetSignalFile(el, &fd);
                xthread_set_notify((void*)(intptr_t)fd); // attach signal fd
                xtimer_init(100);
            },
            [](xThread* ctx) {
                aeEventLoop* el = (aeEventLoop*)ctx->userdata;
                if (el) {
                    // Process events with short timeout to allow thread to exit
                    aeProcessEvents(el, _loop_flag);
                }
            },
            [](xThread* ctx) {
                aeEventLoop* el = (aeEventLoop*)ctx->userdata;
                if (el) {
                    aeDeleteEventLoop(el);
                    ctx->userdata = nullptr;
                }
                xtimer_uninit();
            });

        // 使用
        xthread_post(XTHR_WORKER_GRP1, [](xThread* ctx, std::vector<VariantType>& args) {
            // 这个任务会自动分配到组内队列最短的线程
            std::vector<VariantType> result;
            result.reserve(2);
            result.emplace_back(0);  // 第一个元素是返回码
            result.emplace_back(str_to_pack("success"));  // 使用辅助函数构造 XPackBuff
            return result;
            });
    }
    // Give threads time to initialize their event loops
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    // Start coroutines
    coroutine_run(test_coroutine_with_ae, nullptr);
    coroutine_run(performance_test, nullptr);

    xlog_info("Main thread: Processing RPC results with automatic signal notification");

    // Main loop - 不再需要手动发送信号
    int frame_count = 0;
    while(true) {
        aeProcessEvents(el, _loop_flag);
        xthread_update();               // weakup && exec task

        // 不再需要手动发送信号，系统会自动处理
        if (frame_count % 50 == 0) {
            xlog_info("[Main Thread] System running... frame %d", frame_count);
        }

        //std::this_thread::sleep_for(std::chrono::milliseconds(10));
        frame_count++;
    }

    xlog_info("Demo completed, cleaning up...");

    // Cleanup
    xthread_uninit();
    coroutine_uninit();
    xlog_uninit();
    aeDeleteEventLoop(el);
    xtimer_uninit();

    xlog_info("All resources cleaned up successfully");
    return 0;
}
