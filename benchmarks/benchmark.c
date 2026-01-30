#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

// 内部数据结构
typedef struct BenchmarkNode {
    BenchmarkCase test_case;    // 改为直接存储，而非指针
    BenchResult result;
    struct BenchmarkNode* next;
    int need_free_context;      // 标记是否需要释放context
} BenchmarkNode;

static BenchmarkNode* g_benchmark_list = NULL;
static int g_benchmark_count = 0;

// 高精度计时器
static long long get_time_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency = { 0 };
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000LL) / frequency.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}

// 初始化框架
void benchmark_init(void) {
    g_benchmark_list = NULL;
    g_benchmark_count = 0;
    srand((unsigned int)time(NULL));
}

// 注册测试用例
void benchmark_register(const char* name, BenchmarkFunc func, void* context,
    int warmup_runs, size_t min_runs, size_t max_duration_us) {

    BenchmarkNode* node = (BenchmarkNode*)malloc(sizeof(BenchmarkNode));
    if (!node) return;

    // 初始化测试用例
    node->test_case.name = (char*)malloc(strlen(name) + 1);
    node->test_case.func = func;
    node->test_case.context = context;
    node->test_case.min_runs = min_runs;
    node->test_case.max_duration_us = max_duration_us;
    node->test_case.warmup_runs = warmup_runs;
    node->need_free_context = 0; // 默认不释放
    strcpy(node->test_case.name, name);

    // 初始化结果
    memset(&node->result, 0, sizeof(BenchResult));
    node->result.test_name = node->test_case.name;
    
    // 添加到链表
    node->next = g_benchmark_list;
    g_benchmark_list = node;
    g_benchmark_count++;
}

// 运行单个测试
static void run_benchmark(BenchmarkNode* node) {
    if (!node) return;

    BenchmarkCase* test_case = &node->test_case;
    BenchResult* result = &node->result;

    printf("运行测试: %s\n", test_case->name);

    // 预热运行
    for (int i = 0; i < test_case->warmup_runs; i++) {
        test_case->func(test_case->context);
    }

    // 准备测量
    double* run_times = (double*)malloc(test_case->min_runs * sizeof(double));
    if (!run_times) return;

    size_t actual_runs = 0;
    long long total_duration = 0;
    double min_time = 1e30;
    double max_time = 0;
    double sum = 0;
    double sum_squares = 0;

    // 运行测试
    for (actual_runs = 0; actual_runs < test_case->min_runs; actual_runs++) {
        long long start = get_time_us();
        size_t op_count = test_case->func(test_case->context);
        long long end = get_time_us();

        long long duration = end - start;
        double time_per_op = (op_count > 0) ? (double)duration / op_count : (double)duration;

        run_times[actual_runs] = time_per_op;
        total_duration += duration;
        sum += time_per_op;
        sum_squares += time_per_op * time_per_op;

        if (time_per_op < min_time) min_time = time_per_op;
        if (time_per_op > max_time) max_time = time_per_op;

        // 检查是否超过最大测试时间
        if (total_duration > (long long)test_case->max_duration_us) {
            actual_runs++;
            break;
        }
    }

    // 计算统计结果
    result->runs = actual_runs;
    result->total_time_us = (double)total_duration;
    result->avg_time_us = sum / actual_runs;
    result->min_time_us = min_time;
    result->max_time_us = max_time;
    result->operation_count = 0; // 这里可以根据需要设置

    // 计算标准差
    if (actual_runs > 1) {
        double mean = sum / actual_runs;
        double variance = (sum_squares / actual_runs) - (mean * mean);
        result->stddev_us = sqrt(variance > 0 ? variance : 0);
    }
    else {
        result->stddev_us = 0;
    }

    free(run_times);
}

// 运行所有测试
void benchmark_run_all(void) {
    printf("\n========== 开始基准测试 ==========\n");
    printf("测试用例数: %d\n\n", g_benchmark_count);

    BenchmarkNode* current = g_benchmark_list;
    while (current) {
        run_benchmark(current);
        current = current->next;
    }

    printf("\n========== 所有测试完成 ==========\n");
}

// 运行单个测试
void benchmark_run_single(const char* test_name) {
    BenchmarkNode* current = g_benchmark_list;
    while (current) {
        if (strcmp(current->test_case.name, test_name) == 0) {
            printf("\n========== 运行单个测试: %s ==========\n", test_name);
            run_benchmark(current);
            benchmark_print_results();
            return;
        }
        current = current->next;
    }
    printf("测试 '%s' 未找到\n", test_name);
}

// 打印结果（修正编码问题）
void benchmark_print_results(void) {
    printf("\n%-30s %12s %12s %12s %12s %10s\n",
        "测试名称", "平均(us)", "最小(us)", "最大(us)", "标准差", "波动率%%");
    printf("%-30s %12s %12s %12s %12s %10s\n",
        "------------------------------", "------------", "------------",
        "------------", "------------", "----------");

    BenchmarkNode* current = g_benchmark_list;
    while (current) {
        BenchResult* r = &current->result;
        if (r->runs > 0) {
            double variation = (r->max_time_us - r->min_time_us) / r->min_time_us * 100;
            printf("%-30s %12.2f %12.2f %12.2f %12.2f %10.1f\n",
                r->test_name, r->avg_time_us, r->min_time_us,
                r->max_time_us, r->stddev_us, variation);
        }
        current = current->next;
    }
}

// 导出CSV文件
void benchmark_export_csv(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        printf("无法创建文件: %s\n", filename);
        return;
    }

    fprintf(fp, "TestName,AvgTimeUs,MinTimeUs,MaxTimeUs,StdDevUs,Runs,TotalTimeUs\n");

    BenchmarkNode* current = g_benchmark_list;
    while (current) {
        BenchResult* r = &current->result;
        fprintf(fp, "\"%s\",%.2f,%.2f,%.2f,%.2f,%zu,%.0f\n",
            r->test_name, r->avg_time_us, r->min_time_us,
            r->max_time_us, r->stddev_us, r->runs, r->total_time_us);
        current = current->next;
    }

    fclose(fp);
    printf("结果已导出到: %s\n", filename);
}

// 清理资源
void benchmark_cleanup(void) {
    BenchmarkNode* current = g_benchmark_list;
    while (current) {
        BenchmarkNode* next = current->next;
        if (current->test_case.name) {
            free(current->test_case.name);
        }
        // 如果需要，释放context
        if (current->need_free_context && current->test_case.context) {
            free(current->test_case.context);
        }
        free(current);
        current = next;
    }
    g_benchmark_list = NULL;
    g_benchmark_count = 0;
}