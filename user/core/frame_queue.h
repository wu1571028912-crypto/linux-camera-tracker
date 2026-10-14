#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define MAX_FRAME_QUEUE 4   // 队列最大长度，可根据需要调整

/* 线程间传递 RGB565 的有界 FIFO。stride 单位为像素，每像素 2 字节。
 * push 深拷贝；pop 转移内存所有权。满队列丢新帧，不覆盖旧帧。
 * close 可唤醒消费者，但已入队数据仍可取走；join 使用者后才能 destroy。 */

typedef struct {
    unsigned char *data;    // 帧数据指针（拷贝后的数据）
    int width;
    int height;
    int stride;
    uint64_t queued_ms; /* 原始帧进入软件队列的单调时间，供控制器拒绝旧图像。 */
} Frame;

typedef struct {
    Frame frames[MAX_FRAME_QUEUE];
    int front;
    int rear;
    int count;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;    // 本实现中可能不使用 not_full，但保留
} FrameQueue;

/* 输入 q：有效、未初始化的队列地址；输出已初始化空队列，无返回值。
 * 仅调用一次，不检查 pthread 初始化错误。 */
void frame_queue_init(FrameQueue *q);

/* 输入 q：已初始化且已无并发使用者的队列；释放剩余帧和同步对象，无返回值。 */
void frame_queue_destroy(FrameQueue *q);

/* 输入 q（可 NULL）；标记关闭、唤醒阻塞消费者，无返回值；不丢弃已有帧。 */
void frame_queue_close(FrameQueue *q);

/* 输入 q、data 和正尺寸；stride>=width，data 至少有 height*stride*2 字节。
 * 成功复制并入队返回 0；满/关闭/参数错误/分配失败返回 -1。
 * 不等待空位；成功后原 data 仍归调用方所有，入队副本由队列管理。 */
int frame_queue_push(FrameQueue *q, const unsigned char *data,
                     int width, int height, int stride);

/* 输入 q，输出 out_frame；空且未关闭时阻塞。返回 0 时调用者必须
 * free(out_frame->data)；返回 -1 表示参数错误或队列关闭且耗尽，不取得所有权。 */
int frame_queue_pop(FrameQueue *q, Frame *out_frame);

#endif

