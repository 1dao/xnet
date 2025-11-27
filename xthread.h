// xthread.h - 线程池与跨线程RPC
// 支持预定义线程ID，协程内同步调用，复用xcoroutine框架

#ifndef _XTHREAD_H
#define _XTHREAD_H

#include "xcoroutine.h"
#include "xpack.h"
#include "xmutex.h"
#include "xerrno.h"
#include "xlog.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// ============================================================================
// 预定义线程ID (0-99)
// ============================================================================

#define XTHR_INVALID        0
#define XTHR_MAIN           1   // 主线程
#define XTHR_REDIS          2   // Redis线程
#define XTHR_MYSQL          3   // MySQL线程
#define XTHR_LOG            4   // 日志线程
#define XTHR_IO             5   // IO线程
#define XTHR_COMPUTE        6   // 计算线程
#define XTHR_WORKER_BASE    10  // 工作线程起始

#define XTHR_MAX            100

// 错误码
#define XTHR_ERR_NO_THREAD  -101
#define XTHR_ERR_QUEUE_FULL -102
#define XTHR_ERR_NOT_INIT   -103

#ifdef __cplusplus

#include <functional>
#include <atomic>
#include <queue>
#include <vector>

struct XThreadContext;

// 任务函数类型: (ctx, args) -> results
using XThreadFunc = std::function<std::vector<VariantType>(XThreadContext*, std::vector<VariantType>&)>;

// 任务结构
struct XThreadTask {
    XThreadFunc              func;
    std::vector<VariantType> args;
    uint32_t                 wait_id;        // 用于RPC回调(复用xcoroutine的wait机制)
    int                      source_thread;

    XThreadTask() : wait_id(0), source_thread(0) {}
};

// ============================================================================
// 任务队列(线程安全，支持IOCP/socketpair唤醒)
// ============================================================================

class XTaskQueue {
public:
    XTaskQueue();
    ~XTaskQueue();

    bool init();
    void uninit();
    bool push(XThreadTask&& task);
    std::vector<XThreadTask> pop_all();
    bool wait(int timeout_ms);

#ifdef _WIN32
    HANDLE get_iocp() const { return iocp_; }
#else
    int get_notify_fd() const { return fds_[1]; }
#endif

private:
    std::queue<XThreadTask> queue_;
    xMutex                  lock_;
    int                     pending_;
#ifdef _WIN32
    HANDLE                  iocp_;
#else
    int                     fds_[2];
#endif
};

// ============================================================================
// 线程上下文
// ============================================================================

struct XThreadContext {
    int                     id;
    const char*             name;
    std::atomic<bool>       running;
    XTaskQueue              queue;
    void*                   userdata;

#ifdef _WIN32
    HANDLE                  handle;
#else
    pthread_t               handle;
#endif

    void (*on_init)(XThreadContext*);
    void (*on_update)(XThreadContext*);
    void (*on_cleanup)(XThreadContext*);

    XThreadContext();
};

// ============================================================================
// 全局API
// ============================================================================

bool xthread_init();
void xthread_uninit();

// 注册工作线程(创建新线程)
bool xthread_register(int id, const char* name,
                      void (*on_init)(XThreadContext*) = nullptr,
                      void (*on_update)(XThreadContext*) = nullptr,
                      void (*on_cleanup)(XThreadContext*) = nullptr);

// 注册主线程(不创建新线程)
bool xthread_register_main(int id, const char* name);

void xthread_unregister(int id);
XThreadContext* xthread_get(int id);
int xthread_current_id();
XThreadContext* xthread_current();

// 主线程调用：处理任务队列
int xthread_update();

// 异步投递(不等待结果)
bool xthread_post(int target_id, XThreadFunc func, std::vector<VariantType> args = {});

// ============================================================================
// RPC调用 - 在协程中使用，返回 xAwaiter
// ============================================================================

// 核心RPC函数
xAwaiter xthread_rpc(int target_id, XThreadFunc func, std::vector<VariantType> args);

// 便捷模板接口: xthread_pcall(XTHR_REDIS, func, arg1, arg2, ...)
template<typename... Args>
xAwaiter xthread_pcall(int target_id, XThreadFunc func, Args&&... args) {
    std::vector<VariantType> packed;
    if constexpr (sizeof...(args) > 0) {
        (packed.emplace_back(std::forward<Args>(args)), ...);
    }
    return xthread_rpc(target_id, std::move(func), std::move(packed));
}

// 检查结果
inline bool xthread_ok(std::vector<VariantType>& result) {
    if (result.empty()) return false;
    return xpack_cast<int>(result[0]) == 0;
}

inline int xthread_retcode(std::vector<VariantType>& result) {
    if (result.empty()) return -999;
    return xpack_cast<int>(result[0]);
}

#endif // __cplusplus
#endif // _XTHREAD_H
