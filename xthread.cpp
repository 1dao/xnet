// xthread.cpp - 线程池实现

#include "xthread.h"
#include "xlog.h"
#include <cstring>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#endif

// ============================================================================
// 全局状态
// ============================================================================

static XThreadContext*  g_threads[XTHR_MAX] = {nullptr};
static xMutex           g_lock;
static bool             g_init = false;

#ifdef _WIN32
static DWORD            g_tls = TLS_OUT_OF_INDEXES;
#define XTHR_COMPLETION_KEY ((ULONG_PTR)-1)
#else
static pthread_key_t    g_tls;
static pthread_once_t   g_tls_once = PTHREAD_ONCE_INIT;
static void tls_init_once() { pthread_key_create(&g_tls, nullptr); }
#endif

// ============================================================================
// TLS
// ============================================================================

static void tls_set(int id) {
#ifdef _WIN32
    TlsSetValue(g_tls, (LPVOID)(intptr_t)id);
#else
    pthread_setspecific(g_tls, (void*)(intptr_t)id);
#endif
}

static int tls_get() {
#ifdef _WIN32
    return (int)(intptr_t)TlsGetValue(g_tls);
#else
    return (int)(intptr_t)pthread_getspecific(g_tls);
#endif
}

// ============================================================================
// XThreadContext
// ============================================================================

XThreadContext::XThreadContext()
    : id(0), name(nullptr), running(false), userdata(nullptr), handle(0),
      on_init(nullptr), on_update(nullptr), on_cleanup(nullptr) {}

// ============================================================================
// XTaskQueue
// ============================================================================

XTaskQueue::XTaskQueue() : pending_(0) {
#ifdef _WIN32
    iocp_ = nullptr;
#else
    fds_[0] = fds_[1] = -1;
#endif
}

XTaskQueue::~XTaskQueue() {
    uninit();
}

bool XTaskQueue::init() {
    xnet_mutex_init(&lock_);
    pending_ = 0;

#ifdef _WIN32
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    return iocp_ != nullptr;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) != 0) return false;
    fcntl(fds_[0], F_SETFL, O_NONBLOCK);
    fcntl(fds_[1], F_SETFL, O_NONBLOCK);
    return true;
#endif
}

void XTaskQueue::uninit() {
#ifdef _WIN32
    if (iocp_) { CloseHandle(iocp_); iocp_ = nullptr; }
#else
    if (fds_[0] >= 0) { close(fds_[0]); close(fds_[1]); fds_[0] = fds_[1] = -1; }
#endif
    xnet_mutex_destroy(&lock_);
}

bool XTaskQueue::push(XThreadTask&& task) {
    xnet_mutex_lock(&lock_);
    queue_.push(std::move(task));

    if (!pending_) {
#ifdef _WIN32
        if (PostQueuedCompletionStatus(iocp_, 0, XTHR_COMPLETION_KEY, nullptr))
            pending_ = 1;
#else
        if (send(fds_[0], "!", 1, 0) > 0)
            pending_ = 1;
#endif
    }
    xnet_mutex_unlock(&lock_);
    return true;
}

std::vector<XThreadTask> XTaskQueue::pop_all() {
    std::vector<XThreadTask> tasks;
    xnet_mutex_lock(&lock_);
    pending_ = 0;
    while (!queue_.empty()) {
        tasks.push_back(std::move(queue_.front()));
        queue_.pop();
    }
    xnet_mutex_unlock(&lock_);
    return tasks;
}

bool XTaskQueue::wait(int timeout_ms) {
#ifdef _WIN32
    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD trans; ULONG_PTR key; LPOVERLAPPED ov;
    return GetQueuedCompletionStatus(iocp_, &trans, &key, &ov, timeout) != FALSE;
#else
    char buf[64];
    while (recv(fds_[1], buf, sizeof(buf), 0) > 0);

    struct pollfd pfd = { fds_[1], POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms) > 0;
#endif
}

// ============================================================================
// 工作线程函数
// ============================================================================

static void process_tasks(XThreadContext* ctx) {
    auto tasks = ctx->queue.pop_all();

    for (auto& task : tasks) {
        std::vector<VariantType> result;
        try {
            result = task.func(ctx, task.args);
            // 插入成功码到开头
            result.insert(result.begin(), (int)0);
        } catch (const std::exception& e) {
            xlog_err("Thread[%d] task error: %s", ctx->id, e.what());
            result.clear();
            result.push_back((int)XNET_CORO_EXCEPT);
        } catch (...) {
            result.clear();
            result.push_back((int)XNET_UNKNOWN_ERROR);
        }

        // RPC回调：恢复调用者协程
        if (task.wait_id != 0) {
            coroutine_resume(task.wait_id, std::move(result));
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall worker_func(void* arg) {
#else
static void* worker_func(void* arg) {
#endif
    XThreadContext* ctx = (XThreadContext*)arg;
    tls_set(ctx->id);

    xlog_info("Thread[%d:%s] started", ctx->id, ctx->name);
    // ctx->name 存在实时ctx->name,不存在使用"THR:%d", %d替换线程ID
    if (ctx->name) {
        xlog_set_thread_name(ctx->name);
    } else {
        char name[32];
        snprintf(name, sizeof(name), "THR:%d", ctx->id);
        xlog_set_thread_name(name);
    }

    if (ctx->on_init) ctx->on_init(ctx);

    while (ctx->running) {
        if (ctx->queue.wait(100)) {
            process_tasks(ctx);
        }
        if (ctx->on_update) ctx->on_update(ctx);
    }

    // 处理剩余任务
    process_tasks(ctx);

    if (ctx->on_cleanup) ctx->on_cleanup(ctx);

    xlog_info("Thread[%d:%s] stopped", ctx->id, ctx->name);

#ifdef _WIN32
    return 0;
#else
    return nullptr;
#endif
}

// ============================================================================
// 全局API
// ============================================================================

bool xthread_init() {
    if (g_init) return true;

    xnet_mutex_init(&g_lock);
    memset(g_threads, 0, sizeof(g_threads));

#ifdef _WIN32
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) return false;
#else
    pthread_once(&g_tls_once, tls_init_once);
#endif

    g_init = true;
    return true;
}

void xthread_uninit() {
    if (!g_init) return;

    for (int i = 0; i < XTHR_MAX; i++) {
        if (g_threads[i]) xthread_unregister(i);
    }

#ifdef _WIN32
    if (g_tls != TLS_OUT_OF_INDEXES) { TlsFree(g_tls); g_tls = TLS_OUT_OF_INDEXES; }
#endif

    xnet_mutex_destroy(&g_lock);
    g_init = false;
}

bool xthread_register(int id, const char* name,
                      void (*on_init)(XThreadContext*),
                      void (*on_update)(XThreadContext*),
                      void (*on_cleanup)(XThreadContext*)) {
    if (id <= 0 || id >= XTHR_MAX || !g_init) return false;

    xnet_mutex_lock(&g_lock);
    if (g_threads[id]) { xnet_mutex_unlock(&g_lock); return false; }

    XThreadContext* ctx = new XThreadContext();
    ctx->id = id;
    ctx->name = name;
    ctx->on_init = on_init;
    ctx->on_update = on_update;
    ctx->on_cleanup = on_cleanup;
    ctx->running = true;

    if (!ctx->queue.init()) {
        delete ctx;
        xnet_mutex_unlock(&g_lock);
        return false;
    }

    g_threads[id] = ctx;

#ifdef _WIN32
    ctx->handle = (HANDLE)_beginthreadex(nullptr, 0, worker_func, ctx, 0, nullptr);
    if (!ctx->handle) {
        ctx->queue.uninit();
        delete ctx;
        g_threads[id] = nullptr;
        xnet_mutex_unlock(&g_lock);
        return false;
    }
#else
    if (pthread_create(&ctx->handle, nullptr, worker_func, ctx) != 0) {
        ctx->queue.uninit();
        delete ctx;
        g_threads[id] = nullptr;
        xnet_mutex_unlock(&g_lock);
        return false;
    }
#endif

    xnet_mutex_unlock(&g_lock);
    return true;
}

bool xthread_register_main(int id, const char* name) {
    if (id <= 0 || id >= XTHR_MAX || !g_init) return false;

    xnet_mutex_lock(&g_lock);
    if (g_threads[id]) { xnet_mutex_unlock(&g_lock); return false; }

    XThreadContext* ctx = new XThreadContext();
    ctx->id = id;
    ctx->name = name;
    ctx->running = true;
    ctx->handle = 0;

    if (!ctx->queue.init()) {
        delete ctx;
        xnet_mutex_unlock(&g_lock);
        return false;
    }

    g_threads[id] = ctx;
    tls_set(id);

    xnet_mutex_unlock(&g_lock);
    return true;
}

void xthread_unregister(int id) {
    if (id <= 0 || id >= XTHR_MAX) return;

    xnet_mutex_lock(&g_lock);
    XThreadContext* ctx = g_threads[id];
    if (!ctx) { xnet_mutex_unlock(&g_lock); return; }

    ctx->running = false;
    g_threads[id] = nullptr;
    xnet_mutex_unlock(&g_lock);

    if (ctx->handle) {
#ifdef _WIN32
        WaitForSingleObject(ctx->handle, INFINITE);
        CloseHandle(ctx->handle);
#else
        pthread_join(ctx->handle, nullptr);
#endif
    }

    ctx->queue.uninit();
    delete ctx;
}

XThreadContext* xthread_get(int id) {
    if (id <= 0 || id >= XTHR_MAX) return nullptr;
    return g_threads[id];
}

int xthread_current_id() { return tls_get(); }

XThreadContext* xthread_current() { return xthread_get(tls_get()); }

int xthread_update() {
    XThreadContext* ctx = xthread_current();
    if (!ctx) return 0;

    auto tasks = ctx->queue.pop_all();
    int count = 0;

    for (auto& task : tasks) {
        std::vector<VariantType> result;
        try {
            result = task.func(ctx, task.args);
            result.insert(result.begin(), (int)0);
        } catch (...) {
            result.clear();
            result.push_back((int)XNET_CORO_EXCEPT);
        }

        if (task.wait_id != 0) {
            coroutine_resume(task.wait_id, std::move(result));
        }
        count++;
    }
    return count;
}

bool xthread_post(int target_id, XThreadFunc func, std::vector<VariantType> args) {
    XThreadContext* target = xthread_get(target_id);
    if (!target || !target->running) return false;

    XThreadTask task;
    task.func = std::move(func);
    task.args = std::move(args);
    task.wait_id = 0;
    task.source_thread = tls_get();

    return target->queue.push(std::move(task));
}

// ============================================================================
// RPC调用 - 核心实现
// ============================================================================

xAwaiter xthread_rpc(int target_id, XThreadFunc func, std::vector<VariantType> args) {
    XThreadContext* target = xthread_get(target_id);
    if (!target || !target->running) {
        return xAwaiter(XTHR_ERR_NO_THREAD);
    }

    int co_id = coroutine_self_id();
    if (co_id == -1) {
        return xAwaiter(XNET_NOT_IN_COROUTINE);
    }

    // 创建awaiter获取wait_id
    xAwaiter awaiter;
    uint32_t wait_id = awaiter.wait_id();
    if (wait_id == 0) {
        return xAwaiter(XNET_NOT_IN_COROUTINE);
    }

    // 构造任务
    XThreadTask task;
    task.func = std::move(func);
    task.args = std::move(args);
    task.wait_id = wait_id;
    task.source_thread = tls_get();

    // 投递任务
    if (!target->queue.push(std::move(task))) {
        return xAwaiter(XTHR_ERR_QUEUE_FULL);
    }

    return awaiter;
}
