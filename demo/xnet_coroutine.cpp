// main.cpp
#include <iostream>
#include <thread>
#include <chrono>
#include <coroutine>
#include "xcoroutine.h"
#include <iostream>

// 用户协程函数 - 现在可以使用 co_await!
void* my_coroutine_function(void* arg) {
    // 在堆上创建任务对象
    return new SimpleTask([arg]() -> SimpleTask {
        void* user_arg = arg;
        std::cout << "Coroutine started with arg: " << (const char*)user_arg << std::endl;

        // 获取当前协程ID
        int my_id = coroutine_self_id();
        std::cout << "My coroutine ID is: " << my_id << std::endl;

        // 直接运行一些代码
        std::cout << "Running initial code..." << std::endl;

        // 使用 co_await 等待参数 - 这里会挂起
        void* received_param = co_await 0;
        my_id = coroutine_self_id();
        std::cout << "Received first parameter: " << my_id << "     params:" << (const char*)received_param << std::endl;
        std::cout << "Still in coroutine ID: " << coroutine_self_id() << std::endl;

        // 继续运行
        std::cout << "Running more code..." << std::endl;

        // 再次等待参数
        void* another_param = co_await 0;
        std::cout << "Received second parameter: " << my_id << "    params:" << (const char*)another_param << std::endl;

        std::cout << "Coroutine finished, ID: " << coroutine_self_id() << std::endl;

        co_return;
        }());
}

void* my_coroutine_function2(void* arg) {
    // 在堆上创建任务对象
    return new SimpleTask([arg]() -> SimpleTask {
        std::cout << "======== Coroutine started with arg: " << (const char*)arg << std::endl;

        // 获取当前协程ID
        int my_id = coroutine_self_id();
        std::cout << "======== My coroutine ID is: " << my_id << std::endl;

        // 直接运行一些代码
        std::cout << "======== Running initial code..." << std::endl;

        // 使用 co_await 等待参数 - 这里会挂起
        void* received_param = co_await 0;
        my_id = coroutine_self_id();
        std::cout << "======== Received first parameter: " << my_id << "     params:" << (const char*)received_param << std::endl;
        std::cout << "======== Still in coroutine ID: " << coroutine_self_id() << std::endl;

        // 继续运行
        std::cout << "======== Running more code..." << std::endl;

        // 再次等待参数
        void* another_param = co_await 0;
        std::cout << "======== Received second parameter: " << my_id << "    params:" << (const char*)another_param << std::endl;

        std::cout << "======== Coroutine finished, ID: " << coroutine_self_id() << std::endl;

        co_return;
        }());
}

// 使用 variant 的多类型参数协程函数
void* variant_coroutine_function(void* arg) {
    VariantCoroutineArgs* variant_args = static_cast<VariantCoroutineArgs*>(arg);

    return new SimpleTask([variant_args]() -> SimpleTask {
        std::cout << "=== Variant-based Coroutine ===" << std::endl;
        int my_id = coroutine_self_id();
        std::cout << "My coroutine ID is: " << my_id << std::endl;

        // 打印所有参数
        variant_args->print_args();

        // 安全地获取各种类型的参数
        if (auto name = variant_args->get_arg<std::string>(0)) {
            std::cout << "Name: " << *name << std::endl;
        }

        if (auto age = variant_args->get_arg<int>(1)) {
            std::cout << "Age: " << *age << std::endl;
        }

        if (auto score = variant_args->get_arg<double>(2)) {
            std::cout << "Score: " << *score << std::endl;
        }

        if (auto active = variant_args->get_arg<bool>(3)) {
            std::cout << "Active: " << (*active ? "true" : "false") << std::endl;
        }

        void* runtime_param = co_await 0;
        std::cout << "Runtime parameter: " << static_cast<const char*>(runtime_param) << std::endl;

        std::cout << "=== Variant-based Coroutine finished ===" << std::endl;

        // 清理参数
        // delete variant_args;
        co_return;
        }());
}

// 更复杂的多类型参数示例
void* complex_variant_coroutine(void* arg) {
    VariantCoroutineArgs* variant_args = static_cast<VariantCoroutineArgs*>(arg);

    return new SimpleTask([variant_args]() -> SimpleTask {
        std::cout << "=== Complex Variant Coroutine ===" << std::endl;

        // 使用 visit 来处理 variant
        for (size_t i = 0; i < variant_args->size(); ++i) {
            std::cout << "Parameter " << i << ": ";
            std::visit([](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, int>) {
                    std::cout << "Integer: " << value;
                }
                else if constexpr (std::is_same_v<T, double>) {
                    std::cout << "Double: " << value;
                }
                else if constexpr (std::is_same_v<T, bool>) {
                    std::cout << "Boolean: " << (value ? "true" : "false");
                }
                else if constexpr (std::is_same_v<T, std::string>) {
                    std::cout << "String: \"" << value << "\"";
                }
                else if constexpr (std::is_same_v<T, const char*>) {
                    std::cout << "C-string: \"" << value << "\"";
                }
                else {
                    std::cout << "Unknown type";
                }
                }, variant_args->args[i]);
            std::cout << std::endl;
        }

        // 处理不同类型的运行时参数
        void* runtime_data = co_await 0;
        std::cout << "Processing runtime data..." << std::endl;

        // 可以在这里根据 runtime_data 做不同的处理

        std::cout << "=== Complex Variant Coroutine finished ===" << std::endl;

        //delete variant_args;
        co_return;
        }());
}

// 计算相关的协程示例
void* calculator_coroutine(void* arg) {
    VariantCoroutineArgs* variant_args = static_cast<VariantCoroutineArgs*>(arg);

    return new SimpleTask([variant_args]() -> SimpleTask {
        std::cout << "=== Calculator Coroutine ===" << std::endl;

        // 获取操作数
        auto operand1 = variant_args->get_arg<double>(0);
        auto operand2 = variant_args->get_arg<double>(1);
        auto operation = variant_args->get_arg<std::string>(2);

        if (operand1 && operand2 && operation) {
            double result = 0.0;
            if (*operation == "add") {
                result = *operand1 + *operand2;
            }
            else if (*operation == "subtract") {
                result = *operand1 - *operand2;
            }
            else if (*operation == "multiply") {
                result = *operand1 * *operand2;
            }
            else if (*operation == "divide" && *operand2 != 0) {
                result = *operand1 / *operand2;
            }

            std::cout << "Calculation: " << *operand1 << " " << *operation
                << " " << *operand2 << " = " << result << std::endl;
        }

        // 等待更多计算指令
        void* new_operation = co_await 0;
        if (new_operation) {
            std::cout << "New operation requested: " << static_cast<const char*>(new_operation) << std::endl;
        }

        std::cout << "=== Calculator Coroutine finished ===" << std::endl;

        //delete variant_args;
        co_return;
        }());
}


int main() {
    coroutine_init();

    // 启动协程 - 会立即运行直到第一个 co_await
    int coroutine_id1 = coroutine_run(my_coroutine_function, (void*)"hello");
    int coroutine_id10 = coroutine_run(my_coroutine_function2, (void*)"world");

    if (coroutine_id1 >= 0) {
        std::cout << "Main: Started coroutine with ID: " << coroutine_id1 << std::endl;

        // 此时协程已经在第一个 co_await 处挂起
        // 通过ID传递参数并恢复执行
        coroutine_resume(coroutine_id1, (void*)"first param");
        coroutine_resume(coroutine_id10, (void*)"first2 param");

        // 协程会运行到下一个 co_await 处再次挂起
        coroutine_resume(coroutine_id1, (void*)"second param");

    }

    std::cout << "\n=== Testing variant-based multi-type coroutine ===" << std::endl;
    VariantCoroutineArgs args();
    int coroutine_id2 = coroutine_run_variant(variant_coroutine_function, {
        std::string("John Doe"),
        25,
        95.5,
        true
        });
    if (coroutine_id2 >= 0) {
        coroutine_resume(coroutine_id2, (void*)"Additional information");
    }

    std::cout << "\n=== Testing complex variant coroutine ===" << std::endl;
    int coroutine_id3 = coroutine_run_variant(complex_variant_coroutine, {
        "Hello World",            // const char*
        std::string("C++20"),     // std::string
        100,                      // int
        3.14159,                  // double
        false                     // bool
        });
    if (coroutine_id3 >= 0) {
        coroutine_resume(coroutine_id3, (void*)"Processing complete");
    }

    std::cout << "\n=== Testing calculator coroutine ===" << std::endl;
    int coroutine_id4 = coroutine_run_variant(calculator_coroutine, {
        15.0,                     // double - operand1
        3.0,                      // double - operand2
        std::string("divide")     // string - operation
        });
    if (coroutine_id4 >= 0) {
        coroutine_resume(coroutine_id4, (void*)"calculate more");
    }

    std::cout << "\n=== Final status ===" << std::endl;
    std::cout << "Coroutine1 status: " << coroutine_is_done(coroutine_id1) << std::endl;
    std::cout << "Coroutine2 status: " << coroutine_is_done(coroutine_id2) << std::endl;
    std::cout << "Coroutine3 status: " << coroutine_is_done(coroutine_id3) << std::endl;
    std::cout << "Coroutine4 status: " << coroutine_is_done(coroutine_id4) << std::endl;
    std::cout << "Active coroutines: " << coroutine_get_active_count() << std::endl;


    std::cout << "Main: finish coroutine1 status: " << coroutine_is_done(coroutine_id1) << std::endl;
    std::cout << "Main: finish coroutine2 status: " << coroutine_is_done(coroutine_id2) << std::endl;

    coroutine_uninit();
    return 0;
}
