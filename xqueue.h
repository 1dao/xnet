#ifndef __XQUEUE_H__
#define __XQUEUE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>  // for size_t
#include <string.h>  // for memcpy

typedef struct {
    void* data;          // 指向动态分配的内存块
    size_t elem_size;    // 单个元素的字节大小
    size_t capacity;     // 队列最大元素数量（注意：实际可用为 capacity - 1）
    size_t front;        // 队头索引
    size_t rear;         // 队尾索引
} xCircleQueue;

static inline int xqueue_circle_init(xCircleQueue* queue, size_t elem_size, size_t capacity) {
    if (!queue || elem_size == 0 || capacity < 2) return -1;
    queue->data = malloc(elem_size * capacity);
    if (!queue->data) return -1;
    queue->elem_size = elem_size;
    queue->capacity = capacity;
    queue->front = 0;
    queue->rear = 0;
    return 0;
}

static inline void xqueue_circle_uninit(xCircleQueue* queue) {
    if (queue && queue->data) {
        free(queue->data);
        queue->data = NULL;
    }
}

static inline bool xqueue_circle_empty(const xCircleQueue* queue) {
    return queue->front == queue->rear;
}

static inline bool xqueue_circle_full(const xCircleQueue* queue) {
    return (queue->rear + 1) % queue->capacity == queue->front;
}

static inline size_t xqueue_circle_size(const xCircleQueue* queue) {
    return (queue->rear + queue->capacity - queue->front) % queue->capacity;
}

// 入队操作
static inline int xqueue_circle_enqueue(xCircleQueue* queue, const void* elem) {
    if (!queue || !elem || xqueue_circle_full(queue)) {
        return 0; // 失败
    }
    char* base = (char*)queue->data;
    memcpy(base + queue->rear * queue->elem_size, elem, queue->elem_size);
    queue->rear = (queue->rear + 1) % queue->capacity;
    return 1; // 成功
}

// 出队操作
// out_elem: 指向接收出队元素的缓冲区（必须已分配足够空间）
static inline int xqueue_circle_dequeue(xCircleQueue* queue, void* out_elem) {
    if (!queue || !out_elem || xqueue_circle_empty(queue)) {
        return 0; // 失败
    }
    char* base = (char*)queue->data;
    memcpy(out_elem, base + queue->front * queue->elem_size, queue->elem_size);
    queue->front = (queue->front + 1) % queue->capacity;
    return 1; // 成功
}

#ifdef __cplusplus
}
#endif

#endif // __XQUEUE_H__