// test_coroutine.cpp
#include "xcoroutine.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <ae.h>
#include "xtimer.h"

// 测试协程 1: 正常执行的协程
xCoroTask test_normal_coroutine(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Coroutine %d: Started normal coroutine: sleep 3s", coro_id);

    // 挂起并等待 100ms
    auto awaiter = coroutine_sleep(3000);
    co_await awaiter;

    xlog_info("Coroutine %d: Resumed after sleep 5s", coro_id);

    // 再次挂起
    co_await coroutine_sleep(5000);

    xlog_info("Coroutine %d: Finished normal execution", coro_id);
    co_return;
}

// 测试协程 2: 会抛出 C++ 异常的协程
xCoroTaskT<std::string> test_exception_coroutine(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Coroutine %d: Started exception coroutine", coro_id);

    try {
        // 模拟一些工作
        co_await coroutine_sleep(50);

        // 故意抛出异常
        if (*(int*)arg == 1) {
            throw std::runtime_error("Test C++ exception thrown intentionally");
        }

        co_return std::string("Success");
    }
    catch (const std::exception& e) {
        xlog_err("Coroutine %d caught exception: %s", coro_id, e.what());
        throw; // 重新抛出，让 promise 捕获
    }

    co_return std::string("Should not reach here");
}

// 测试协程 3: 会触发硬件异常的协程 (访问空指针)
xCoroTask test_hardware_exception_coroutine(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Coroutine %d: Started hardware exception coroutine", coro_id);

    co_await coroutine_sleep(30);

    // 检查是否要触发硬件异常
    if (*(int*)arg == 1) {
        xlog_warn("Coroutine %d: About to trigger hardware exception...", coro_id);

        // 故意触发段错误 (访问空指针)
        // 这在硬件异常保护下应该被捕获
        int* ptr = nullptr;
        *ptr = 42; // 这会触发 SIGSEGV (Linux/Mac) 或 EXCEPTION_ACCESS_VIOLATION (Windows)

        xlog_err("Coroutine %d: Should not reach here after hardware exception", coro_id);
    }
    else {
        xlog_info("Coroutine %d: Running in safe mode, no hardware exception", coro_id);
    }

    co_return;
}

// 测试协程 4: 除零错误
xCoroTask test_divide_by_zero_coroutine(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Coroutine %d: Started divide by zero coroutine", coro_id);

    co_await coroutine_sleep(40);

    if (*(int*)arg == 1) {
        xlog_warn("Coroutine %d: About to trigger divide by zero...", coro_id);

        // 故意触发除零错误
        int a = 10;
        int b = 0;
        int c = a / b; // 这会触发 SIGFPE (Linux/Mac) 或 EXCEPTION_INT_DIVIDE_BY_ZERO (Windows)

        xlog_err("Coroutine %d: Should not reach here after divide by zero", coro_id);
    }

    co_return;
}

// 测试协程 5: 嵌套协程
xCoroTaskT<int> test_nested_coroutine_inner(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Inner coroutine %d: Started", coro_id);

    co_await coroutine_sleep(20);

    int value = *(int*)arg;
    xlog_info("Inner coroutine %d: Returning value %d", coro_id, value);

    co_return value * 2;
}

xCoroTask test_nested_coroutine(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Outer coroutine %d: Started", coro_id);

    // 创建并等待内层协程
    int inner_arg = 21;
    auto inner_task = test_nested_coroutine_inner(&inner_arg);

    // 等待内层协程完成
    try {
        // int result = co_await inner_task;
        int result = co_await std::move(inner_task);
        xlog_info("Outer coroutine %d: Got result from inner coroutine: %d", coro_id, result);
    }
    catch (const std::exception& e) {
        xlog_err("Outer coroutine %d: Exception from inner: %s", coro_id, e.what());
    }

    co_return;
}

// 测试协程 6: 使用 Awaiter 的复杂场景
xCoroTask test_complex_awaiter(void* arg) {
    int coro_id = coroutine_self_id();
    xlog_info("Coroutine %d: Started complex awaiter test", coro_id);

    // 创建多个 awaiter
    xAwaiter awaiter1;
    xAwaiter awaiter2;

    // 设置不同的超时
    awaiter1.set_timeout(100);
    awaiter2.set_timeout(200);

    xlog_info("Coroutine %d: Waiting for awaiter1 (wait_id: %u)", coro_id, awaiter1.wait_id());
    auto result1 = co_await awaiter1;

    if (!result1.empty() && xpack_cast<int>(result1[0]) == -1) {
        xlog_warn("Coroutine %d: awaiter1 timed out", coro_id);
    }
    else {
        xlog_info("Coroutine %d: awaiter1 completed", coro_id);
    }

    co_return;
}

// 测试主函数
void run_coroutine_tests() {
    xlog_info("=== Starting Coroutine Tests ===");

    // 测试 1: 正常协程
    int normal_arg = 0;
    int normal_coro_id = coroutine_run(test_normal_coroutine, &normal_arg);
    xlog_info("Launched normal coroutine with ID: %d", normal_coro_id);

    // 测试 2: 异常协程
    int exception_arg = 1; // 设置为 1 会触发异常
    int exception_coro_id = coroutine_run([](void* arg) -> xCoroTask {
        auto task = test_exception_coroutine(arg);
        auto res = co_await std::move(task);
        xlog_info("Exception coroutine %d: Got result: %s", coroutine_self_id(), res.c_str());
    }, &exception_arg);
    xlog_info("Launched exception coroutine with ID: %d", exception_coro_id);

    //// 测试 3: 硬件异常协程 (安全模式)
    //int hw_safe_arg = 0; // 0 = 安全模式
    //int hw_safe_coro_id = coroutine_run(test_hardware_exception_coroutine, &hw_safe_arg);
    //xlog_info("Launched hardware exception coroutine (safe) with ID: %d", hw_safe_coro_id);

    //// 测试 4: 硬件异常协程 (触发异常模式)
    //int hw_exception_arg = 1; // 1 = 触发异常
    //int hw_exception_coro_id = coroutine_run(test_hardware_exception_coroutine, &hw_exception_arg);
    //xlog_info("Launched hardware exception coroutine (trigger) with ID: %d", hw_exception_coro_id);

    //// 测试 5: 除零错误协程 (安全模式)
    //int div_safe_arg = 0;
    //int div_safe_coro_id = coroutine_run(test_divide_by_zero_coroutine, &div_safe_arg);
    //xlog_info("Launched divide by zero coroutine (safe) with ID: %d", div_safe_coro_id);

    //// 测试 6: 嵌套协程
    //int nested_arg = 0;
    //int nested_coro_id = coroutine_run(test_nested_coroutine, &nested_arg);
    //xlog_info("Launched nested coroutine with ID: %d", nested_coro_id);

    //// 测试 7: 复杂 awaiter 协程
    //int complex_arg = 0;
    //int complex_coro_id = coroutine_run(test_complex_awaiter, &complex_arg);
    //xlog_info("Launched complex awaiter coroutine with ID: %d", complex_coro_id);

    //// 等待一段时间让协程执行
    //xlog_info("Waiting for coroutines to execute...");
    //for (int i = 0; i < 10; i++) {
    //    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    //    // 定期恢复所有协程
    //    coroutine_resume_all();

    //    // 打印活动协程数量
    //    size_t active = coroutine_get_active_count();
    //    xlog_info("Iteration %d: Active coroutines: %zu", i, active);

    //    if (active == 0) {
    //        xlog_info("All coroutines completed");
    //        break;
    //    }
    //}

    // 检查各个协程的状态
    xlog_info("\n=== Checking Coroutine Status ===");
    xlog_info("Normal coroutine %d done: %s", normal_coro_id,
        coroutine_is_done(normal_coro_id) ? "Yes" : "No");
    //xlog_info("Exception coroutine %d done: %s", exception_coro_id,
    //    coroutine_is_done(exception_coro_id) ? "Yes" : "No");
    //xlog_info("HW safe coroutine %d done: %s", hw_safe_coro_id,
    //    coroutine_is_done(hw_safe_coro_id) ? "Yes" : "No");
    //xlog_info("HW exception coroutine %d done: %s", hw_exception_coro_id,
    //    coroutine_is_done(hw_exception_coro_id) ? "Yes" : "No");
    //xlog_info("Divide safe coroutine %d done: %s", div_safe_coro_id,
    //    coroutine_is_done(div_safe_coro_id) ? "Yes" : "No");
    //xlog_info("Nested coroutine %d done: %s", nested_coro_id,
    //    coroutine_is_done(nested_coro_id) ? "Yes" : "No");
    //xlog_info("Complex awaiter coroutine %d done: %s", complex_coro_id,
    //    coroutine_is_done(complex_coro_id) ? "Yes" : "No");
}

// 性能测试：创建大量协程
void performance_test() {
    xlog_info("=== Starting Performance Test ===");

    const int NUM_COROUTINES = 1000;
    std::vector<int> coro_ids;
    coro_ids.reserve(NUM_COROUTINES);

    auto start = std::chrono::high_resolution_clock::now();

    // 创建大量简单协程
    for (int i = 0; i < NUM_COROUTINES; i++) {
        int* value = new int(i);
        int coro_id = coroutine_run([](void* arg) -> xCoroTask {
            int idx = *(int*)arg;
            xlog_debug("Performance coroutine %d started", idx);

            // 模拟一些工作
            co_await coroutine_sleep(10 + (idx % 50));

            xlog_debug("Performance coroutine %d finished", idx);
            delete (int*)arg;
            co_return;
            }, value);

        if (coro_id > 0) {
            coro_ids.push_back(coro_id);
        }
    }

    auto create_end = std::chrono::high_resolution_clock::now();
    auto create_duration = std::chrono::duration_cast<std::chrono::milliseconds>(create_end - start);

    xlog_info("Created %zu coroutines in %lld ms", coro_ids.size(), create_duration.count());

    // 运行所有协程直到完成
    int iterations = 0;
    while (coroutine_get_active_count() > 0 && iterations < 100) {
        coroutine_resume_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        iterations++;

        if (iterations % 10 == 0) {
            xlog_info("Iteration %d: %zu active coroutines remaining",
                iterations, coroutine_get_active_count());
        }
    }

    auto finish_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(finish_end - start);

    xlog_info("All coroutines completed in %lld ms total", total_duration.count());
    xlog_info("=== Performance Test Completed ===");
}

// 主函数
int main() {
    xlog_init(XLOG_DEBUG, true, true, "logs/coroutine.log");
    xtimer_init(1000);
    if (!coroutine_init()) {
        xlog_err("Failed to initialize coroutine system for performance test");
        return -1;
    }

    try {
        // 运行基本功能测试
        run_coroutine_tests();

        std::cout << "\n\n";

        //// 运行性能测试
        //performance_test();
        xlog_info("=== Starting Event Loop ===");
        while (true) {
            aeWait(-1, AE_ALL_EVENTS, 500);
            xtimer_update();
            //xlog_info("Waiting for coroutines to complete...");
        }
        std::cout << "\nAll tests completed successfully!" << std::endl;

        // 清理
        coroutine_uninit();
        xtimer_uninit();
        xlog_info("=== Coroutine Tests Completed ===");
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    catch (...) {
        std::cerr << "Test failed with unknown exception" << std::endl;
        return 1;
    }
}