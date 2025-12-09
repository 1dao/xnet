// xtimer.c
#include "xtimer.h"

typedef struct xTimerNode {
    xHeapMinNode base;

    int id;
    fnOnTime callback;
    void* user_data;
    int repeat_num;
    int repeat_interval;
    char name[32];
} xTimerNode;

typedef struct xTimerSet {
    xHeapMin* timer_heap;
    int next_timer_id;
    long64 current_time;
} xTimerSet;

xTimerNode* xtimer_node_new(int id, long64 timeout, int interval_ms, fnOnTime callback
        , void* ud, int num, const char* name) {
    xTimerNode* timer = (xTimerNode*)malloc(sizeof(xTimerNode));
    timer->base.heap_index = -1;
    timer->base.key = timeout;

    timer->id = id;
    timer->callback = callback;
    timer->user_data = ud;
    timer->repeat_num = num;
    timer->repeat_interval = interval_ms;
    if (name) {
        strncpy(timer->name, name, sizeof(timer->name) - 1);
        timer->name[sizeof(timer->name) - 1] = '\0';
    } else {
        timer->name[0] = '\0';
    }

    return timer;
}

static inline void xtimer_node_del(xTimerNode* timer) {
    free(timer);
}

xTimerSet* xtimer_pool_create(int capacity) {
    capacity = (capacity < 500) ? 1 : capacity;
    xTimerSet* tm = (xTimerSet*)malloc(sizeof(xTimerSet));
    tm->timer_heap = xheapmin_create(capacity, xheapmin_compare);
    tm->next_timer_id = 1;
    tm->current_time = time_get_ms();

    return tm;
}

void xtimer_pool_destroy(xTimerSet* tm) {
    if (!tm) return;
    if (tm->timer_heap) {
        for (int i = 0; i < tm->timer_heap->size; i++) {
            xTimerNode* timer = (xTimerNode*)tm->timer_heap->data[i];
            xtimer_node_del(timer);
        }
        xheapmin_destroy(tm->timer_heap);
    }
    free(tm);
}

xTimerNode* xtimer_create(xTimerSet* tm, int interval_ms, const char* name, fnOnTime callback, void* ud, int repeat_num) {
    if (!tm) return NULL;
    if (repeat_num == -1) {
        repeat_num = 0x7FFFFFFF;
    }
    long64 timeout = tm->current_time + interval_ms;
    xTimerNode* timer = xtimer_node_new(tm->next_timer_id++, timeout, interval_ms, callback, ud, repeat_num, name);
    xheapmin_insert(tm->timer_heap, (xHeapMinNode*)timer);

    //printf("添加定时器: ID=%d, 名称=%s, %dms后过期, 重复间隔=%dms\n",
    //    timer->id, timer->name[0] ? timer->name : "<未命名>",
    //    interval_ms, repeat_num);

    return timer;
}

void xtimer_destroy(xTimerSet* tm, xTimerNode* timer) {
    if (!timer) return;

    if (tm && xheapmin_check(tm->timer_heap, (xHeapMinNode*)timer)) {
        xheapmin_remove(tm->timer_heap, timer->base.heap_index);
    }
    //printf("删除定时器: ID=%d, 名称=%s\n", timer->id, timer->name);

    xtimer_node_del(timer);
}

void timer_refresh(xTimerSet* tm, xTimerNode* timer) {
    if (!tm || !timer || timer->repeat_interval <= 0) return;

    long64 new_expire_time = tm->current_time + timer->repeat_interval;
    xheapmin_refresh(tm->timer_heap, (xHeapMinNode*)timer, new_expire_time);

    printf("重新调度定时器: ID=%d, 下次过期时间=%lld\n",
        timer->id, new_expire_time);
}

int xtimer_poll(xTimerSet* tm) {
    tm->current_time = time_get_ms();

    int triggered_count = 0;
    int next_timeout = 0;
    while (xheapmin_size(tm->timer_heap) > 0) {
        xTimerNode* next_timer = (xTimerNode*)xheapmin_peek(tm->timer_heap);
        if (next_timer->base.key > tm->current_time) {
            next_timeout = (int)(next_timer->base.key - tm->current_time);
            break;
        }

        xTimerNode* expired_timer = next_timer;//(xTimerNode*)xheapmin_extract(tm->timer_heap);
        --expired_timer->repeat_num;
        //printf("定时器触发: ID=%d, 名称=%s, 剩余次数=%d\n",
        //    expired_timer->id, expired_timer->name, expired_timer->repeat_num);

        fnOnTime callback = expired_timer->callback;
        void* ud = expired_timer->user_data;
        if (expired_timer->repeat_num > 0) {
            timer_refresh(tm, expired_timer);
        } else {
            xheapmin_extract(tm->timer_heap);
            xtimer_node_del(expired_timer);
        }
        if (callback) {
            callback(ud);
        }
        if (triggered_count++ > 64)
            break;
    }
    return next_timeout;
}

void xtimer_print(xTimerSet* tm) {
    if (!tm) return;

    printf("\n=== 定时器管理器状态 ===\n");
    printf("当前时间: %lld\n", tm->current_time);
    printf("活动定时器数量: %d\n", xheapmin_size(tm->timer_heap));

    xTimerNode* next_timer = (xTimerNode*)xheapmin_peek(tm->timer_heap);
    if (next_timer) {
        printf("下一个到期定时器: ID=%d, 名称=%s, %lldms后到期\n",
            next_timer->id, next_timer->name,
            next_timer->base.key - tm->current_time);
    }

    printf("所有定时器:\n");
    for (int i = 0; i < tm->timer_heap->size; i++) {
        xTimerNode* timer = (xTimerNode*)tm->timer_heap->data[i];
        printf("  [%d] ID=%d, 名称=%s, 过期时间=%lld (%lldms后), 重复=%d, 堆索引=%d\n",
            i, timer->id, timer->name, timer->base.key,
            timer->base.key - tm->current_time,
            timer->repeat_interval, timer->base.heap_index);
    }
}

#ifdef _WIN32
static _declspec(thread) xTimerSet* _cur = NULL;
#else
static __thread xTimerSet* _cur = NULL;
#endif

void xtimer_init(int cap) {
    if (!_cur)
        _cur = xtimer_pool_create(cap);
}

void xtimer_uninit() {
    if (_cur) {
        xtimer_pool_destroy(_cur);
    }
}

int xtimer_update() {
    if (_cur) {
        return xtimer_poll(_cur);
    } else {
        return 0;
    }
}

void xtimer_show() {
    xtimer_print(_cur);
}

xtimerHandler xtimer_add(int interval_ms, const char* name, fnOnTime callback, void* ud, int repeat_num) {
    if (!_cur) {
        _cur = xtimer_pool_create(100);
    }
    
    return (xtimerHandler)xtimer_create(_cur, interval_ms, name, callback, ud, repeat_num);
}

void xtimer_del(xtimerHandler handler) {
    if (_cur) 
        xtimer_destroy(_cur, (xTimerNode*)handler);
    else
        xtimer_node_del((xTimerNode*)handler);
}
