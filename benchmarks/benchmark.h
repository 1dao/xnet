#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

    // 测试统计数据结构
    typedef struct {
        const char* test_name;     // 测试名称
        double total_time_us;      // 总耗时（微秒）
        double avg_time_us;        // 平均耗时
        double min_time_us;        // 最小耗时
        double max_time_us;        // 最大耗时
        double stddev_us;          // 标准差
        size_t operation_count;    // 操作次数
        size_t runs;               // 运行次数
    } BenchResult;

    // 测试函数原型：返回操作次数（用于计算平均每次操作时间）
    typedef size_t(*BenchmarkFunc)(void* context);

    // 测试用例配置
    typedef struct {
        const char* name;          // 测试名称
        BenchmarkFunc func;        // 测试函数
        void* context;             // 测试上下文
        size_t min_runs;           // 最小运行次数
        size_t max_duration_us;    // 最大测试时间（微秒）
        int warmup_runs;           // 预热次数
    } BenchmarkCase;

    // 框架核心API
    void benchmark_init(void);
    void benchmark_register(const char* name, BenchmarkFunc func, void* context,
        int warmup_runs, size_t min_runs, size_t max_duration_us);
    void benchmark_run_all(void);
    void benchmark_run_single(const char* test_name);
    void benchmark_print_results(void);
    void benchmark_export_csv(const char* filename);
    void benchmark_cleanup(void);

    // 便捷函数：注册测试用例（简化版）
    static inline void benchmark_register_simple(const char* name, BenchmarkFunc func, void* context) {
        benchmark_register(name, func, context, 3, 10, 2000000);
    }

    // 阻止编译器优化的工具宏（Windows兼容版本）
#ifdef _MSC_VER
#define BENCHMARK_NO_OPTIMIZE(value) \
        do { \
            volatile int _dummy; \
            _dummy = (int)((size_t)(value)); \
            (void)_dummy; \
        } while(0)
#else
#define BENCHMARK_NO_OPTIMIZE(value) \
        do { \
            volatile size_t _dummy = (size_t)(value); \
            (void)_dummy; \
        } while(0)
#endif

#ifdef __cplusplus
}
#endif

#endif // BENCHMARK_H