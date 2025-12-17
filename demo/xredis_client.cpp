#include "xredis.h"
#include "xcoroutine.h"
#include "xlog.h"
#include "ae.h"
#include "xtimer.h"
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <map>
#include <unordered_set>
#include "xpack.h"

// 连接状态检查函数
bool check_redis_connected() {
    int total = 0, idle = 0, in_use = 0, initializing = 0;
    int status = xredis_status(&total, &idle, &in_use, &initializing);

    if (status != 0) {
        std::cout << "Redis pool not initialized!" << std::endl;
        return false;
    }

    std::cout << "Redis pool status - Total: " << total
        << ", Idle: " << idle
        << ", In Use: " << in_use
        << ", Initializing: " << initializing << std::endl;

    return idle > 0 || total > 0;
}

// 通用结果处理函数
void process_redis_result(std::vector<VariantType>& result, const std::string& operation) {
    if (result.empty()) {
        std::cout << operation << ": No response received" << std::endl;
        return;
    }

    // 检查错误码
    try {
        int error_code = xpack_cast<int>(result[0]);
        if (error_code != 0) {
            std::cout << operation << " failed with error code: " << error_code << std::endl;
            return;
        }

        std::cout << operation << " success!" << std::endl;

        // 打印返回值
        for (size_t i = 1; i < result.size(); ++i) {
            std::cout << "  Result[" << i << "]: ";

            // 根据实际类型打印
            if (std::holds_alternative<std::string>(result[i])) {
                std::cout << "String: " << std::get<std::string>(result[i]);
            }
            else if (std::holds_alternative<long long>(result[i])) {
                std::cout << "Integer: " << std::get<long long>(result[i]);
            }
            else if (std::holds_alternative<double>(result[i])) {
                std::cout << "Double: " << std::get<double>(result[i]);
            }
            else if (std::holds_alternative<bool>(result[i])) {
                std::cout << "Boolean: " << (std::get<bool>(result[i]) ? "true" : "false");
            }
            else if (std::holds_alternative<std::vector<std::string>>(result[i])) {
                const auto& vec = std::get<std::vector<std::string>>(result[i]);
                std::cout << "Array[" << vec.size() << "]: ";
                for (const auto& item : vec) {
                    std::cout << item << " ";
                }
            }
            else if (std::holds_alternative<std::map<std::string, std::string>>(result[i])) {
                const auto& map = std::get<std::map<std::string, std::string>>(result[i]);
                std::cout << "Map[" << map.size() << "]: ";
                for (const auto& [k, v] : map) {
                    std::cout << k << "=" << v << " ";
                }
            }
            else if (std::holds_alternative<std::unordered_set<std::string>>(result[i])) {
                const auto& set = std::get<std::unordered_set<std::string>>(result[i]);
                std::cout << "Set[" << set.size() << "]: ";
                for (const auto& item : set) {
                    std::cout << item << " ";
                }
            }
            else {
                std::cout << "Unknown type";
            }
            std::cout << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cout << "Error processing result for " << operation << ": " << e.what() << std::endl;
    }
}

// 基础字符串操作测试
void test_basic_string_operations() {
    std::cout << "\n=== Basic String Operations Test ===" << std::endl;

    coroutine_run([](void*) -> xCoroTask {
        try {
            // 1. SET 操作
            std::cout << "1. Testing SET command..." << std::endl;
            auto set_result = co_await xredis_set("test:key1", "Hello, Redis!");
            process_redis_result(set_result, "SET test:key1");

            // 2. GET 操作
            std::cout << "\n2. Testing GET command..." << std::endl;
            auto get_result = co_await xredis_get("test:key1");
            process_redis_result(get_result, "GET test:key1");

            // 3. 设置不存在的键
            std::cout << "\n3. Testing SETNX (SET if not exists) via command..." << std::endl;
            auto setnx_result = co_await xredis_command({ "SET", "test:key2", "value2", "NX" });
            process_redis_result(setnx_result, "SETNX test:key2");

            // 4. 再次尝试SETNX（应该失败）
            std::cout << "\n4. Testing SETNX again (should fail)..." << std::endl;
            auto setnx_again = co_await xredis_command({ "SET", "test:key2", "newvalue", "NX" });
            process_redis_result(setnx_again, "SETNX test:key2 again");

            // 5. 带过期时间的SET
            std::cout << "\n5. Testing SET with EXPIRE..." << std::endl;
            auto setex_result = co_await xredis_command({ "SET", "test:temp", "temporary", "EX", "10" });
            process_redis_result(setex_result, "SETEX test:temp");

        }
        catch (const std::exception& e) {
            std::cout << "Exception in string operations: " << e.what() << std::endl;
        }
        co_return;
    }, nullptr);
}

// 哈希操作测试
void test_hash_operations() {
    std::cout << "\n=== Hash Operations Test ===" << std::endl;

    coroutine_run([](void*) -> xCoroTask {
        try {
            // 1. HSET 多个字段
            std::cout << "1. Testing HSET multiple fields..." << std::endl;
            auto hmset_result = co_await xredis_command({ "HMSET", "test:user:1001",
                "name", "Alice",
                "age", "30",
                "email", "alice@example.com" });
            process_redis_result(hmset_result, "HMSET test:user:1001");

            // 2. HGET 单个字段
            std::cout << "\n2. Testing HGET..." << std::endl;
            auto hget_result = co_await xredis_hget("test:user:1001", "name");
            process_redis_result(hget_result, "HGET test:user:1001 name");

            // 3. HGETALL
            std::cout << "\n3. Testing HGETALL..." << std::endl;
            auto hgetall_result = co_await xredis_hgetall("test:user:1001");
            process_redis_result(hgetall_result, "HGETALL test:user:1001");

            // 4. HDEL 删除字段
            std::cout << "\n4. Testing HDEL..." << std::endl;
            auto hdel_result = co_await xredis_command({ "HDEL", "test:user:1001", "email" });
            process_redis_result(hdel_result, "HDEL test:user:1001 email");

            // 5. HGETALL 再次检查
            std::cout << "\n5. Testing HGETALL after deletion..." << std::endl;
            auto hgetall_again = co_await xredis_hgetall("test:user:1001");
            process_redis_result(hgetall_again, "HGETALL test:user:1001 after deletion");

        }
        catch (const std::exception& e) {
            std::cout << "Exception in hash operations: " << e.what() << std::endl;
        }
        co_return;
    }, nullptr);
}

// 列表和集合操作测试
void test_list_and_set_operations() {
    std::cout << "\n=== List and Set Operations Test ===" << std::endl;

    coroutine_run([](void*) -> xCoroTask {
        try {
            // 1. RPUSH 列表操作
            std::cout << "1. Testing RPUSH..." << std::endl;
            auto rpush_result = co_await xredis_command({ "RPUSH", "test:mylist", "item1", "item2", "item3" });
            process_redis_result(rpush_result, "RPUSH test:mylist");

            // 2. LRANGE 获取列表范围
            std::cout << "\n2. Testing LRANGE..." << std::endl;
            auto lrange_result = co_await xredis_command({ "LRANGE", "test:mylist", "0", "-1" });
            process_redis_result(lrange_result, "LRANGE test:mylist");

            // 3. SADD 集合操作
            std::cout << "\n3. Testing SADD..." << std::endl;
            auto sadd_result = co_await xredis_command({ "SADD", "test:myset", "member1", "member2", "member3" });
            process_redis_result(sadd_result, "SADD test:myset");

            // 4. SMEMBERS 获取集合所有成员
            std::cout << "\n4. Testing SMEMBERS..." << std::endl;
            auto smembers_result = co_await xredis_command({ "SMEMBERS", "test:myset" });
            process_redis_result(smembers_result, "SMEMBERS test:myset");

            // 5. SISMEMBER 检查成员是否存在
            std::cout << "\n5. Testing SISMEMBER..." << std::endl;
            auto sismember_result = co_await xredis_command({ "SISMEMBER", "test:myset", "member2" });
            process_redis_result(sismember_result, "SISMEMBER test:myset member2");

        }
        catch (const std::exception& e) {
            std::cout << "Exception in list/set operations: " << e.what() << std::endl;
        }
        co_return;
    }, nullptr);
}

// 键管理操作测试
void test_key_management() {
    std::cout << "\n=== Key Management Test ===" << std::endl;

    coroutine_run([](void*) -> xCoroTask {
        try {
            // 1. 设置几个测试键
            std::cout << "1. Setting up test keys..." << std::endl;
            co_await xredis_set("test:key:a", "value_a");
            co_await xredis_set("test:key:b", "value_b");
            co_await xredis_set("test:key:c", "value_c");

            // 2. KEYS 命令
            std::cout << "\n2. Testing KEYS pattern..." << std::endl;
            auto keys_result = co_await xredis_command({ "KEYS", "test:key:*" });
            process_redis_result(keys_result, "KEYS test:key:*");

            // 3. EXISTS 命令
            std::cout << "\n3. Testing EXISTS..." << std::endl;
            auto exists_result = co_await xredis_command({ "EXISTS", "test:key:a", "test:key:b", "test:key:d" });
            process_redis_result(exists_result, "EXISTS test:key:a,b,d");

            // 4. TYPE 命令
            std::cout << "\n4. Testing TYPE..." << std::endl;
            auto type_result = co_await xredis_command({ "TYPE", "test:key:a" });
            process_redis_result(type_result, "TYPE test:key:a");

            // 5. TTL 命令（为键设置过期时间）
            std::cout << "\n5. Testing EXPIRE and TTL..." << std::endl;
            auto expire_result = co_await xredis_command({ "EXPIRE", "test:key:a", "60" });
            process_redis_result(expire_result, "EXPIRE test:key:a 60");

            auto ttl_result = co_await xredis_command({ "TTL", "test:key:a" });
            process_redis_result(ttl_result, "TTL test:key:a");

            // 6. DEL 命令
            std::cout << "\n6. Testing DEL..." << std::endl;
            auto del_result = co_await xredis_command({ "DEL", "test:key:b", "test:key:c" });
            process_redis_result(del_result, "DEL test:key:b,c");

        }
        catch (const std::exception& e) {
            std::cout << "Exception in key management: " << e.what() << std::endl;
        }
        co_return;
    }, nullptr);
}

// 高级特性测试（管道、事务等）
void test_advanced_features() {
    std::cout << "\n=== Advanced Features Test ===" << std::endl;

    coroutine_run([](void*) -> xCoroTask {
        try {
            // 1. 使用MULTI/EXEC进行事务测试
            std::cout << "1. Testing MULTI/EXEC transaction..." << std::endl;

            // 开始事务
            auto multi_result = co_await xredis_command({ "MULTI" });
            process_redis_result(multi_result, "MULTI");

            // 在事务中执行多个命令
            co_await xredis_command({ "SET", "test:tx:key1", "tx_value1" });
            co_await xredis_command({ "INCR", "test:tx:counter" });
            co_await xredis_command({ "HSET", "test:tx:hash", "field", "value" });

            // 执行事务
            auto exec_result = co_await xredis_command({ "EXEC" });
            process_redis_result(exec_result, "EXEC");

            // 2. 检查事务结果
            std::cout << "\n2. Checking transaction results..." << std::endl;
            auto check1 = co_await xredis_get("test:tx:key1");
            process_redis_result(check1, "GET test:tx:key1");

            auto check2 = co_await xredis_command({ "GET", "test:tx:counter" });
            process_redis_result(check2, "GET test:tx:counter");

            // 3. 清理测试数据
            std::cout << "\n3. Cleaning up test data..." << std::endl;
            co_await xredis_command({ "DEL", "test:tx:key1", "test:tx:counter", "test:tx:hash" });

        }
        catch (const std::exception& e) {
            std::cout << "Exception in advanced features: " << e.what() << std::endl;
        }
        co_return;
    }, nullptr);
}

// 性能测试：并发操作
void test_concurrent_performance() {
    std::cout << "\n=== Concurrent Performance Test ===" << std::endl;

    // 记录开始时间
    long64 start_time = time_get_ms();

    // 创建多个并发协程
    const int num_tasks = 20;
    std::atomic<int> completed_tasks{ 0 };
    struct xargs {
        int i;
        long64 time_ms;
        int task_num;
        std::atomic<int>& completed_tasks;
        xargs(int i, std::atomic<int>& tnum, long64 time, int total) 
            : i(i), completed_tasks(tnum), time_ms(time), task_num(total) {}
    };
    xargs t1(0, completed_tasks, start_time, num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        t1.i = i;
        coroutine_run([](void* arg) -> xCoroTask {
            xargs* args = (xargs*)arg;
            int i = args->i;
            std::atomic<int>& completed_tasks = args->completed_tasks;
            try {
                std::string key = "perf:key:" + std::to_string(i);
                std::string value = "value_" + std::to_string(i) + "_" + std::to_string(rand() % 1000);

                // 执行SET
                auto set_result = co_await xredis_set(key, value);

                // 执行GET
                auto get_result = co_await xredis_get(key);

                // 检查结果
                if (get_result.size() > 1) {
                    auto opt_val = xpack_cast_optional<std::string>(get_result, 1);
                    if (opt_val && *opt_val == value) {
                        // 成功计数
                        // completed_tasks.fetch_add(1);
                    }
                }

                // 清理
                co_await xredis_command({ "DEL", key });

            } catch (const std::exception& e) {
                std::cout << "Task " << i << " failed: " << e.what() << std::endl;
            }
            co_return;
        }, (void*)&t1);
    }

    // 等待所有任务完成（在实际应用中可能需要更复杂的同步机制）
    coroutine_run([](void* arg) -> xCoroTask {
        xargs* args = (xargs*)arg;
        std::atomic<int>& completed_tasks = args->completed_tasks;

        // 等待一段时间让任务完成
        co_await coroutine_sleep(5000);

        long64 end_time = time_get_ms();
        auto duration = end_time-args->time_ms;

        std::cout << "Performance test completed in " << duration << "ms" << std::endl;
        std::cout << "Tasks completed: " << completed_tasks.load() << "/" << num_tasks << std::endl;
        std::cout << "Average time per operation: " << (duration / (num_tasks * 2.0)) << "ms" << std::endl;

        co_return;
    }, (void*)&t1);
}

// 错误处理测试
void test_error_handling() {
    std::cout << "\n=== Error Handling Test ===" << std::endl;

    coroutine_run([](void*) -> xCoroTask {
        try {
            // 1. 对不存在的键进行GET
            std::cout << "1. Testing GET on non-existent key..." << std::endl;
            auto get_result = co_await xredis_get("test:nonexistent");
            process_redis_result(get_result, "GET test:nonexistent");

            // 2. 错误的命令语法
            std::cout << "\n2. Testing invalid command syntax..." << std::endl;
            auto invalid_result = co_await xredis_command({ "SET", "key" }); // 缺少值参数
            process_redis_result(invalid_result, "SET with missing value");

            // 3. 类型错误：对字符串键执行HSET
            std::cout << "\n3. Testing type mismatch error..." << std::endl;
            co_await xredis_set("test:string_key", "just_a_string");
            auto type_error = co_await xredis_hset("test:string_key", "field", "value");
            process_redis_result(type_error, "HSET on string key");

            // 4. 连接超时测试（通过设置极短的超时）
            std::cout << "\n4. Note: Connection timeout test would require Redis server to be unreachable" << std::endl;
            std::cout << "   (Skipped in normal test environment)" << std::endl;

        }
        catch (const std::exception& e) {
            std::cout << "Exception in error handling test: " << e.what() << std::endl;
        }
        co_return;
    }, nullptr);
}


void subscribe_func(const std::string& channel, const std::string& type, std::vector<VariantType>& resp) {
    process_redis_result(resp, "Receive publish my_channel result:");
}

// 主测试函数，按顺序运行所有测试
void run_all_tests() {
    std::cout << "=== Starting xRedis Client Tests ===" << std::endl;

    // 顺序执行所有测试
    coroutine_run([](void*) -> xCoroTask {
        // 检查连接状态
        while (!check_redis_connected()) {
            std::cout << "Redis client connecting." << std::endl;
            co_await coroutine_sleep(500);
        }
         
        // 订阅频道
        auto res = co_await xredis_subscribe("news_channel", [](const std::string& type,
            const std::string& channel,
            std::vector<VariantType>& msg) {
                std::cout << "Received: " << std::get<std::string>(msg[0]) << std::endl;
            });

        // 模式订阅
        co_await xredis_subscribe("news_*", [](const std::string& type,
            const std::string& pattern,
            std::vector<VariantType>& msg) {
                std::cout << "Pattern match: " << pattern << std::endl;
            });

        // 发布消息
        auto result = co_await xredis_publish("news_channel", "Hello World!");

        // 取消订阅
        auto pres = co_await xredis_unsubscribe("news_channel");

        // 运行各个测试模块
        test_basic_string_operations();
        co_await coroutine_sleep(1000);

        test_hash_operations();
        co_await coroutine_sleep(1000);

        test_list_and_set_operations();
        co_await coroutine_sleep(1000);

        test_key_management();
        co_await coroutine_sleep(1000);

        test_advanced_features();
        co_await coroutine_sleep(1000);

        test_error_handling();
        co_await coroutine_sleep(1000);

        test_concurrent_performance();
        co_await coroutine_sleep(6000); // 等待性能测试完成

        // 最终清理
        std::cout << "\n=== Final Cleanup ===" << std::endl;
        auto cleanup_result = co_await xredis_command({ "DEL",
            "test:key1", "test:key2", "test:temp",
            "test:user:1001", "test:mylist", "test:myset",
            "test:key:a", "test:key:b", "test:key:c",
            "test:tx:key1", "test:tx:counter", "test:tx:hash",
            "test:string_key" });

        std::cout << "Cleanup completed. Test keys removed." << std::endl;
        std::cout << "\n=== All Tests Completed ===" << std::endl;

        // 显示最终状态
        check_redis_connected();

        co_return;
    }, nullptr);
}

int main() {
    // 初始化事件循环
    aeEventLoop* el = aeCreateEventLoop(1024);
    if (!el) {
        std::cerr << "Failed to create event loop!" << std::endl;
        return -1;
    }

    // 初始化日志系统
    xlog_init(XLOG_DEBUG, true, true, "logs/xredis_demo.log");
    xlog_set_show_thread_name(true);

    // 初始化协程系统
    if (!coroutine_init()) {
        std::cerr << "Failed to initialize coroutine system!" << std::endl;
        return -1;
    }

    // 初始化定时器
    xtimer_init(500);

    // 初始化Redis连接池
    RedisConnConfig config;
    config.ip = "127.0.0.1";
    config.port = 6379;
    config.password = ""; // 如果Redis需要密码，请在此设置
    config.db_index = 1;
    config.use_resp3 = true;

    int init_ret = xredis_init(config, 10); // 使用10个连接
    if (init_ret != 0) {
        std::cerr << "Failed to initialize Redis pool: " << init_ret << std::endl;

        // 尝试简化的初始化方式
        init_ret = xredis_init("127.0.0.1", 6379, 10);
        if (init_ret != 0) {
            std::cerr << "Failed to initialize Redis with simple config: " << init_ret << std::endl;

            // 清理已初始化的资源
            coroutine_uninit();
            xlog_uninit();
            xtimer_uninit();
            aeDeleteEventLoop(el);
            return -1;
        }
    }

    std::cout << "Redis pool initialized successfully!" << std::endl;

    // 运行所有测试
    run_all_tests();

    while (true) {
        aeProcessEvents(el, AE_ALL_EVENTS);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 清理资源
    std::cout << "\nCleaning up resources..." << std::endl;
    xredis_deinit();
    coroutine_uninit();
    xlog_uninit();
    xtimer_uninit();
    aeDeleteEventLoop(el);

    std::cout << "Demo completed successfully!" << std::endl;
    return 0;
}