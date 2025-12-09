#include "xtimer.h"
#include "benchmark.h"
#include <stdlib.h>
#include <string.h>

// 测试上下文结构
typedef struct {
    int timer_count;
    int interval_ms;
    const char** names;
} TimerTestContext;

// 测试1：纯添加定时器性能
static size_t bench_pure_addition(void* context) {
    TimerTestContext* ctx = (TimerTestContext*)context;

    // 避免在计时循环内进行内存分配和字符串格式化
    for (int i = 0; i < ctx->timer_count; ++i) {
        xtimerHandler h = xtimer_add(ctx->interval_ms,ctx->names[i], NULL, NULL, 1);
        // 防止编译器优化掉整个调用
        BENCHMARK_NO_OPTIMIZE(h);
    }

    return ctx->timer_count; // 返回操作次数
}

// 测试2：添加后立即删除
static size_t bench_add_delete_cycle(void* context) {
    TimerTestContext* ctx = (TimerTestContext*)context;

    for (int i = 0; i < ctx->timer_count; ++i) {
        xtimerHandler h = xtimer_add(ctx->interval_ms, ctx->names[i], NULL, NULL, 1);
        xtimer_del(h);
        BENCHMARK_NO_OPTIMIZE(h);
    }

    return ctx->timer_count * 2; // 添加+删除算2次操作
}

// 测试3：不同时间间隔混合
static size_t bench_mixed_intervals(void* context) {
    TimerTestContext* ctx = (TimerTestContext*)context;
    int intervals[] = { 100, 250, 500, 1000, 2000 };

    for (int i = 0; i < ctx->timer_count; ++i) {
        int interval = intervals[i % 5];
        xtimerHandler h = xtimer_add(interval, ctx->names[i], NULL, NULL, 1);
        BENCHMARK_NO_OPTIMIZE(h);
    }

    return ctx->timer_count;
}

// 测试4：堆大小对性能的影响
static size_t bench_heap_size_impact(void* context) {
    TimerTestContext* ctx = (TimerTestContext*)context;

    // 先创建基础堆大小
    for (int i = 0; i < ctx->timer_count / 2; ++i) {
        xtimer_add(1000, ctx->names[i], NULL, NULL, 1);
    }

    // 测试在已有N个定时器的堆中添加新定时器
    for (int i = ctx->timer_count / 2; i < ctx->timer_count; ++i) {
        xtimerHandler h = xtimer_add(1000, ctx->names[i], NULL, NULL, 1);
        BENCHMARK_NO_OPTIMIZE(h);
    }

    return ctx->timer_count / 2; // 只计算后一半的操作
}

// 初始化测试数据
static TimerTestContext* create_test_context(int timer_count) {
    TimerTestContext* ctx = (TimerTestContext*)malloc(sizeof(TimerTestContext));
    ctx->timer_count = timer_count;
    ctx->interval_ms = 1000;

    // 预生成所有名称
    ctx->names = (const char**)malloc(timer_count * sizeof(char*));
    for (int i = 0; i < timer_count; ++i) {
        char* name = (char*)malloc(32);
        snprintf(name, 32, "timer_%d", i);
        ctx->names[i] = name;
    }

    return ctx;
}

// 清理测试数据
static void cleanup_test_context(TimerTestContext* ctx) {
    if (!ctx) return;

    // 清理名称
    for (int i = 0; i < ctx->timer_count; ++i) {
        free((void*)ctx->names[i]);
    }
    free(ctx->names);
    free(ctx);
}

void register_timer_benchmarks(int timer_count) {
    TimerTestContext* ctx = create_test_context(timer_count);
    if (!ctx) return;

    char test_name[64];
    snprintf(test_name, sizeof(test_name), "PureAdd_%d", timer_count);
    benchmark_register(test_name, bench_pure_addition, ctx, 3, 10, 2000000);

    snprintf(test_name, sizeof(test_name), "AddDeleteCycle_%d", timer_count);
    benchmark_register(test_name, bench_add_delete_cycle, ctx, 3, 10, 2000000);

    snprintf(test_name, sizeof(test_name), "MixedInterval_%d", timer_count);
    benchmark_register(test_name, bench_mixed_intervals, ctx, 2, 8, 2000000);

    snprintf(test_name, sizeof(test_name), "HeapSizeImpact_%d", timer_count);
    benchmark_register(test_name, bench_heap_size_impact, ctx, 2, 8, 2000000);
}

// 运行定时器性能测试套件
void run_timer_benchmark_suite(void) {
    printf("=== 定时器性能基准测试套件 ===\n\n");

    // 测试不同规模
    int test_sizes[] = { 100, 1000, 5000, 10000 };

    for (size_t i = 0; i < sizeof(test_sizes) / sizeof(test_sizes[0]); i++) {
        printf("--- 测试规模: %d 个定时器 ---\n", test_sizes[i]);
        register_timer_benchmarks(test_sizes[i]);
    }

    // 运行所有测试
    benchmark_run_all();
    benchmark_print_results();
    benchmark_export_csv("timer_performance.csv");

    // 这里需要清理所有测试上下文（实际实现中需要更复杂的管理）
}

#include "benchmark.h"
#include "xtimer.h"

// 声明测试套件
void run_timer_benchmark_suite(void);

int main(int argc, char** argv) {
    // 初始化基准测试框架
    benchmark_init();

    // 初始化定时器系统
    xtimer_init(20000);

    printf("========================================\n");
    printf("    通用基准测试框架演示 - xtimer性能测试\n");
    printf("========================================\n\n");

    // 运行定时器测试套件
    run_timer_benchmark_suite();

    // 你也可以在这里添加其他组件的测试

    // 清理
    benchmark_cleanup();
    xtimer_uninit();

    return 0;
}
