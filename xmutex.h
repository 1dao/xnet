#ifndef XMUTEX_H
#define XMUTEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    // ============================================================================
    // 平台检测
    // ============================================================================

#if defined(_WIN32)
#define XMUTEX_WINDOWS 1
#include <windows.h>
#include <intrin.h>
#elif defined(__linux__)
#define XMUTEX_LINUX 1
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#elif defined(__APPLE__)
#define XMUTEX_MACOS 1
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#else
#define XMUTEX_GENERIC 1
#include <pthread.h>
#include <time.h>
#endif

// ============================================================================
// 原子操作封装 - 统一接口
// ============================================================================

#if defined(XMUTEX_WINDOWS)
    static inline int atomic_compare_exchange(volatile int* ptr, int* expected, int desired) {
        int old_expected = *expected;
        *expected = (int)_InterlockedCompareExchange((volatile long*)ptr, (long)desired, (long)old_expected);
        return (*expected == old_expected);
    }

    static inline int atomic_exchange(volatile int* ptr, int value) {
        return (int)_InterlockedExchange((volatile long*)ptr, (long)value);
    }

    static inline void atomic_store(volatile int* ptr, int value) {
        _InterlockedExchange((volatile long*)ptr, (long)value);
    }

    static inline int atomic_load(volatile int* ptr) {
        _ReadWriteBarrier();
        return *ptr;
    }

    // CPU 暂停指令
    static inline void cpu_pause(void) {
        YieldProcessor();
    }

#elif defined(__GNUC__) || defined(__clang__)
    // GCC/Clang 原子操作
    static inline int atomic_compare_exchange(volatile int* ptr, int* expected, int desired) {
        return __atomic_compare_exchange_n(ptr, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    static inline int atomic_exchange(volatile int* ptr, int value) {
        return __atomic_exchange_n(ptr, value, __ATOMIC_ACQ_REL);
    }

    static inline void atomic_store(volatile int* ptr, int value) {
        __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
    }

    static inline int atomic_load(volatile int* ptr) {
        return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
    }

    // CPU 暂停指令
    static inline void cpu_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        __asm__ __volatile__("" ::: "memory");
#endif
    }

#else
    // 通用的原子操作实现
#warning "Using generic atomic operations, performance may be suboptimal"

    static inline int atomic_compare_exchange(volatile int* ptr, int* expected, int desired) {
        int old_value = *ptr;
        if (old_value == *expected) {
            *ptr = desired;
            return 1;
        }
        else {
            *expected = old_value;
            return 0;
        }
    }

    static inline int atomic_exchange(volatile int* ptr, int value) {
        int old = *ptr;
        *ptr = value;
        return old;
    }

    static inline void atomic_store(volatile int* ptr, int value) {
        *ptr = value;
    }

    static inline int atomic_load(volatile int* ptr) {
        return *ptr;
    }

    static inline void cpu_pause(void) {
        for (volatile int i = 0; i < 100; i++);
    }
#endif

    // ============================================================================
    // 时间获取函数（独立于统计锁）
    // ============================================================================

    static inline unsigned long xnet_mutex_current_time_us(void) {
#ifdef _WIN32
        static LARGE_INTEGER frequency = { 0 };
        if (frequency.QuadPart == 0) {
            QueryPerformanceFrequency(&frequency);
        }

        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return (unsigned long)((counter.QuadPart * 1000000) / frequency.QuadPart);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned long)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
#endif
    }

    // ============================================================================
    // 基础互斥锁实现
    // ============================================================================

    typedef struct {
        volatile int locked;
    } xnet_mutex_t;

    static inline void xnet_mutex_init(xnet_mutex_t* mutex) {
        atomic_store(&mutex->locked, 0);
    }

    static inline void xnet_mutex_lock(xnet_mutex_t* mutex) {
        int expected = 0;
        int desired = 1;

        while (!atomic_compare_exchange(&mutex->locked, &expected, desired)) {
            expected = 0;
            cpu_pause();
        }
    }

    /**
     * @brief 尝试锁定互斥锁（非阻塞）
     * @return 0-成功，非0-失败
     */
    static inline int xnet_mutex_trylock(xnet_mutex_t* mutex) {
        int expected = 0;
        int desired = 1;
        return atomic_compare_exchange(&mutex->locked, &expected, desired) ? 0 : -1;
    }

    /**
     * @brief 解锁互斥锁
     */
    static inline void xnet_mutex_unlock(xnet_mutex_t* mutex) {
        atomic_store(&mutex->locked, 0);
    }

    /**
     * @brief 销毁互斥锁
     */
    static inline void xnet_mutex_destroy(xnet_mutex_t* mutex) {
        atomic_store(&mutex->locked, 0);
    }

    /**
     * @brief 检查互斥锁是否被锁定
     * @return 1-已锁定，0-未锁定
     */
    static inline int xnet_mutex_is_locked(xnet_mutex_t* mutex) {
        return atomic_load(&mutex->locked) != 0;
    }

    // ============================================================================
    // 自适应互斥锁实现
    // ============================================================================

    // 自适应互斥锁：在原子锁和系统锁之间自动切换
    typedef struct {
        // 原子锁部分
        volatile int atomic_lock;

        // 系统锁部分
#ifdef _WIN32
        CRITICAL_SECTION sys_lock;
#else
        pthread_mutex_t sys_lock;
#endif

        // 统计信息
        unsigned int spin_count;
        unsigned int total_locks;
        int use_system_lock;  // 是否使用系统锁
    } xnet_adp_mutex_t;

    /**
     * @brief 初始化自适应互斥锁
     */
    static inline void xnet_mutex_adp_init(xnet_adp_mutex_t* mutex) {
        mutex->atomic_lock = 0;
        mutex->spin_count = 0;
        mutex->total_locks = 0;
        mutex->use_system_lock = 0;

#ifdef _WIN32
        InitializeCriticalSection(&mutex->sys_lock);
#else
        pthread_mutex_init(&mutex->sys_lock, NULL);
#endif
    }

    /**
     * @brief 基于统计的自适应决策
     */
    static inline int xnet_mutex_should_use_system_lock(xnet_adp_mutex_t* mutex) {
        // 如果总锁次数太少，继续使用原子锁
        if (mutex->total_locks < 100) return 0;

        // 如果自旋次数过多，切换到系统锁
        float spin_ratio = (float)mutex->spin_count / mutex->total_locks;
        return (spin_ratio > 0.1f); // 超过10%的自旋率就使用系统锁
    }

    /**
     * @brief 锁定自适应互斥锁
     */
    static inline void xnet_mutex_adp_lock(xnet_adp_mutex_t* mutex) {
        mutex->total_locks++;

        if (mutex->use_system_lock) {
            // 使用系统锁
#ifdef _WIN32
            EnterCriticalSection(&mutex->sys_lock);
#else
            pthread_mutex_lock(&mutex->sys_lock);
#endif
            return;
        }

        // 尝试原子锁
        int max_spins = 1000; // 最大自旋次数
        for (int i = 0; i < max_spins; i++) {
            int expected = 0;
            int desired = 1;

            if (atomic_compare_exchange(&mutex->atomic_lock, &expected, desired)) {
                return; // 成功获取锁
            }

            mutex->spin_count++;
            cpu_pause();

            // 检查是否应该切换到系统锁
            if (xnet_mutex_should_use_system_lock(mutex)) {
                mutex->use_system_lock = 1;
                break;
            }
        }

        // 自旋失败，使用系统锁
#ifdef _WIN32
        EnterCriticalSection(&mutex->sys_lock);
#else
        pthread_mutex_lock(&mutex->sys_lock);
#endif
    }

    /**
     * @brief 尝试锁定自适应互斥锁
     */
    static inline int xnet_mutex_adp_trylock(xnet_adp_mutex_t* mutex) {
        mutex->total_locks++;

        if (mutex->use_system_lock) {
#ifdef _WIN32
            return TryEnterCriticalSection(&mutex->sys_lock) ? 0 : -1;
#else
            return pthread_mutex_trylock(&mutex->sys_lock);
#endif
        }

        // 尝试原子锁
        int expected = 0;
        int desired = 1;
        if (atomic_compare_exchange(&mutex->atomic_lock, &expected, desired)) {
            return 0;
        }

        mutex->spin_count++;
        return -1;
    }

    /**
     * @brief 解锁自适应互斥锁
     */
    static inline void xnet_mutex_adp_unlock(xnet_adp_mutex_t* mutex) {
        if (mutex->use_system_lock) {
#ifdef _WIN32
            LeaveCriticalSection(&mutex->sys_lock);
#else
            pthread_mutex_unlock(&mutex->sys_lock);
#endif
        }
        else {
            atomic_store(&mutex->atomic_lock, 0);
        }
    }

    /**
     * @brief 销毁自适应互斥锁
     */
    static inline void xnet_mutex_adp_destroy(xnet_adp_mutex_t* mutex) {
#ifdef _WIN32
        DeleteCriticalSection(&mutex->sys_lock);
#else
        pthread_mutex_destroy(&mutex->sys_lock);
#endif
    }

    /**
     * @brief 获取自适应锁统计信息
     */
    static inline void xnet_mutex_adp_stats(xnet_adp_mutex_t* mutex,
        unsigned int* total_locks,
        unsigned int* spin_count,
        int* using_system_lock) {
        if (total_locks) *total_locks = mutex->total_locks;
        if (spin_count) *spin_count = mutex->spin_count;
        if (using_system_lock) *using_system_lock = mutex->use_system_lock;
    }

    // ============================================================================
    // 统计互斥锁实现（用于性能监控）- 简化版
    // ============================================================================

    typedef struct {
        xnet_mutex_t mutex;
        unsigned long lock_count;
        unsigned long contention_count;
    } xnet_stats_mutex_t;

    /**
     * @brief 初始化统计互斥锁
     */
    static inline void xnet_mutex_stats_init(xnet_stats_mutex_t* smutex) {
        xnet_mutex_init(&smutex->mutex);
        smutex->lock_count = 0;
        smutex->contention_count = 0;
    }

    /**
     * @brief 锁定统计互斥锁
     */
    static inline void xnet_mutex_stats_lock(xnet_stats_mutex_t* smutex) {
        smutex->lock_count++;

        // 首先尝试快速获取
        if (xnet_mutex_trylock(&smutex->mutex) == 0) {
            return;
        }

        // 快速获取失败，统计竞争
        smutex->contention_count++;

        // 使用正常的锁
        xnet_mutex_lock(&smutex->mutex);
    }

    /**
     * @brief 解锁统计互斥锁
     */
    static inline void xnet_mutex_stats_unlock(xnet_stats_mutex_t* smutex) {
        xnet_mutex_unlock(&smutex->mutex);
    }

    /**
     * @brief 销毁统计互斥锁
     */
    static inline void xnet_mutex_stats_destroy(xnet_stats_mutex_t* smutex) {
        xnet_mutex_destroy(&smutex->mutex);
    }

    /**
     * @brief 获取统计信息
     */
    static inline void xnet_mutex_get_stats(xnet_stats_mutex_t* smutex,
        unsigned long* total_locks,
        unsigned long* contentions,
        double* contention_ratio) {
        if (total_locks) *total_locks = smutex->lock_count;
        if (contentions) *contentions = smutex->contention_count;

        if (contention_ratio) {
            *contention_ratio = (smutex->lock_count > 0) ?
                (double)smutex->contention_count / smutex->lock_count : 0.0;
        }
    }

    /**
     * @brief 重置统计信息
     */
    static inline void xnet_mutex_reset_stats(xnet_stats_mutex_t* smutex) {
        smutex->lock_count = 0;
        smutex->contention_count = 0;
    }

    // ============================================================================
    // 向后兼容的宏定义
    // ============================================================================

    // 为了兼容旧的xlog代码
#define xMutex xnet_mutex_t
#define xnet_mutex_uninit xnet_mutex_destroy

#ifdef __cplusplus
}
#endif

#endif // XMUTEX_H