#ifndef _XTHREAD_H
#define _XTHREAD_H
/* xthread.h - 线程池与跨线程RPC
** 支持预定义线程ID，协程内同步调用，复用xcoroutine框架*/

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#include <atomic>

#include "xcoroutine.h"
#include "xpack.h"
#include "xmutex.h"
#include "xerrno.h"
#include "xlog.h"

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
#define XTHR_WORKER_GRP1    10  // 工作线程起始
#define XTHR_WORKER_GRP2    20  // 工作线程组1
#define XTHR_WORKER_GRP3    30  // 工作线程组2

#define XTHR_MAX            100
#define XTHR_GROUP_MAX      20

// 错误码
#define XTHR_ERR_NO_THREAD  -101
#define XTHR_ERR_QUEUE_FULL -102
#define XTHR_ERR_NOT_INIT   -103

#ifdef __cplusplus

#include <functional>
#include <queue>
#include <vector>

struct xThread;

enum ThreadSelStrategy {       // 组线程选择策略
    XTHSTRATEGY_LEAST_QUEUE,   // 最少队列（默认）
    XTHSTRATEGY_ROUND_ROBIN,   // 轮询
    XTHSTRATEGY_RANDOM         // 随机
};

// 任务函数类型: (ctx, args) -> results
using XThreadFunc = std::function<std::vector<VariantType>(xThread*, std::vector<VariantType>&)>;

// ============================================================================
// 全局API
// ============================================================================

bool xthread_init();
void xthread_uninit();

// 注册工作线程(创建新线程)
bool xthread_register(int id, bool xwait_, const char* name,
                      void (*on_init)(xThread*) = nullptr,
                      void (*on_update)(xThread*) = nullptr,
                      void (*on_cleanup)(xThread*) = nullptr);

// 注册线程组
bool xthread_register_group(int base_id, int count, ThreadSelStrategy strategy, bool xwait_,
                           const char* name_pattern,
                           void (*on_init)(xThread*) = nullptr,
                           void (*on_update)(xThread*) = nullptr,
                           void (*on_cleanup)(xThread*) = nullptr);

// 注册主线程(不创建新线程)
bool xthread_register_main(int id, bool xwait_, const char* name);

void xthread_unregister(int id);
xThread* xthread_get(int id);
int xthread_current_id();
xThread* xthread_current();
int xthread_set_notify(void* fd);

// 主线程调用：处理任务队列
int xthread_update();

// 异步投递(不等待结果)
bool xthread_rawpost(int target_id, XThreadFunc func, std::vector<VariantType> args = {});

template<typename... Args>
bool xthread_post(int target_id, XThreadFunc func, Args&&... args) {
    std::vector<VariantType> packed;
    if constexpr (sizeof...(args) > 0) {
        (packed.emplace_back(std::forward<Args>(args)), ...);
    }
    return xthread_rawpost(target_id, std::move(func), std::move(packed));
}

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

// ============================================================================
// 任务结构
// ============================================================================
enum xthrTaskType {
    XTHR_TASK_NORMAL = 0,    // 普通任务
    XTHR_TASK_RESUME = 1     // 协程恢复任务
};

struct xthrTask {
    xthrTaskType             type;
    XThreadFunc              func;
    std::vector<VariantType> args;
    uint32_t                 wait_id;        // 用于RPC回调(复用xcoroutine的wait机制)
    int                      source_thread;

    xthrTask() : type(XTHR_TASK_NORMAL), wait_id(0), source_thread(0) {}

    static xthrTask make_normal(XThreadFunc func, std::vector<VariantType> args = {}) {
        xthrTask task;
        task.type = XTHR_TASK_NORMAL;
        task.func = std::move(func);
        task.args = std::move(args);
        return task;
    }

    static xthrTask make_resume(uint32_t wait_id, std::vector<VariantType> result) {
        xthrTask task;
        task.type = XTHR_TASK_RESUME;
        task.wait_id = wait_id;
        task.args = std::move(result);
        return task;
    }
};

// ============================================================================
// 任务队列(线程安全，支持IOCP/socketpair唤醒)
// ============================================================================

class xthrQueue {
public:
    xthrQueue(bool xwait_);
    ~xthrQueue();

    bool init();
    void uninit();
    bool push(xthrTask&& task, int* out_new_size = nullptr);
    std::vector<xthrTask> pop_all();
    bool wait(int timeout_ms);

#ifdef _WIN32
    HANDLE get_iocp() const { return iocp_; }
    void set_iocp(HANDLE iocp) { iocp_ = iocp; }
#else
    int get_notify_fd() const { return fds_[0]; }
    void set_notify_fd(int fd) { fds_[0] = fd; }
#endif
    void set_xwait(bool wait) { xwait_ = wait; }
    bool get_xwait() const { return xwait_; }

private:
    std::queue<xthrTask>    queue_;
    xMutex                  lock_;
    int                     pending_;
    bool                    xwait_;
#ifdef _WIN32
    HANDLE                  iocp_;
#else
    int                     fds_[2];
#endif
};

// ============================================================================
// 线程上下文
// ============================================================================
class xThreadSet;
struct xThread {
    int                     id;
    const char* name;
    std::atomic<bool>       running;
    xthrQueue               queue;
    void* userdata;
    xThreadSet* group;

#ifdef _WIN32
    HANDLE                  handle;
#else
    pthread_t               handle;
#endif

    void (*on_init)(xThread*);
    void (*on_update)(xThread*);
    void (*on_cleanup)(xThread*);

    xThread(bool xwait_ = false);
};

// ============================================================================
// 线程组
// ============================================================================
class xThreadSet {
private:
    int                     group_id_;
    int                     strategy_;                    // 负载均衡策略
    const char* name_;
    xThread* threads_[XTHR_GROUP_MAX];     // 线程指针数组
    std::atomic<int>        thread_count_;               // 原子计数
    std::atomic<int>        queue_sizes_[XTHR_GROUP_MAX]; // 原子数组：每个线程的队列大小
    std::atomic<int>        next_index_;                 // 原子轮询索引

public:
    xThreadSet(int group_id, int strategy, const char* name)
        : group_id_(group_id),
        strategy_(strategy),
        name_(name),
        thread_count_(0),
        next_index_(0) {
        memset(threads_, 0, sizeof(threads_));

        for (int i = 0; i < XTHR_GROUP_MAX; i++) {
            queue_sizes_[i].store(0, std::memory_order_relaxed);
        }
    }

    xThreadSet(const xThreadSet&) = delete;
    xThreadSet& operator=(const xThreadSet&) = delete;
    xThreadSet(xThreadSet&&) = delete;
    xThreadSet& operator=(xThreadSet&&) = delete;

    int size() const { return thread_count_.load(std::memory_order_relaxed); }
    int get_group_id() const { return group_id_; }
    const char* get_name() const { return name_; }

    bool add_thread(xThread* thread);
    xThread* select_thread();
    xThread* get_thread(int index);
    void update_queue_size(int id, int size);
};

#endif // __cplusplus
#endif // _XTHREAD_H