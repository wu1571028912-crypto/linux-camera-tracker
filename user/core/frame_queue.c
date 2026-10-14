/* 公开所有权/阻塞契约见 frame_queue.h。push 深拷贝，pop 后消费者负责 free。
 * queued_ms 是复制完成时的软件单调时间，不是曝光时间。
 * 满队列拒绝新帧，因此慢消费者可能一直处理旧帧；本次仅说明，不修改此策略。
 */
#define _POSIX_C_SOURCE 200809L
#include "frame_queue.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* 初始化空队列以及配套互斥锁和条件变量。 */
void frame_queue_init(FrameQueue *q)
{
    q->front = q->rear = 0;
    q->count = 0;
    q->closed = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

/*
 * 标记队列不再接收新帧，并唤醒所有等待中的生产者和消费者。
 * 队列中已经保存的帧仍可由消费者取出。
 */
void frame_queue_close(FrameQueue *q)
{
    if (q == NULL)
        return;
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

/* 释放队列仍持有的帧内存，并销毁互斥锁和条件变量。 */
void frame_queue_destroy(FrameQueue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->count > 0) {
        free(q->frames[q->front].data);
        q->front = (q->front + 1) % MAX_FRAME_QUEUE;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/*
 * 复制一帧 RGB565 数据并压入队列。
 * 队列关闭、队列已满、参数非法或内存分配失败时返回 -1，否则返回 0。
 */
int frame_queue_push(FrameQueue *q, const unsigned char *data,
                     int width, int height, int stride)
{
    size_t size;
    unsigned char *copy;

    if (q == NULL || data == NULL || width <= 0 || height <= 0 || stride < width)
        return -1;

    pthread_mutex_lock(&q->lock);

    if (q->closed || q->count >= MAX_FRAME_QUEUE) {
        // 队列满，直接丢弃
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    // 分配内存并拷贝数据
    size = (size_t)height * (size_t)stride * 2U;
    copy = malloc(size);
    if (!copy) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    memcpy(copy, data, size);

    q->frames[q->rear].data = copy;
    q->frames[q->rear].width = width;
    q->frames[q->rear].height = height;
    q->frames[q->rear].stride = stride;
    {
        struct timespec now;
        q->frames[q->rear].queued_ms = clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
            (uint64_t)now.tv_sec * 1000U + now.tv_nsec / 1000000U : 0;
    }
    q->rear = (q->rear + 1) % MAX_FRAME_QUEUE;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/*
 * 阻塞等待并弹出一帧；队列关闭且没有剩余帧时返回 -1。
 * 成功后 Frame.data 的所有权转交调用者，调用者负责 free。
 */
int frame_queue_pop(FrameQueue *q, Frame *out_frame)
{
    if (q == NULL || out_frame == NULL)
        return -1;

    pthread_mutex_lock(&q->lock);

    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    *out_frame = q->frames[q->front];
    q->front = (q->front + 1) % MAX_FRAME_QUEUE;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

