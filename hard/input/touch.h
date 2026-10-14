#ifndef TOUCH_H
#define TOUCH_H

#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>

#include "display_transform.h"

/* 输入仅支持 BTN_TOUCH、ABS_X/Y、SYN_REPORT；raw_x/y 当前按 LCD 像素解释，
 * 未做 evdev 原始量程标定，也未支持 ABS_MT_*。选择结果 x/y 已换算为原图。
 * 顺序：init -> pthread_create(touch_run) -> take_selection -> stop -> join -> close。
 * pending 只保留最新一次尚未消费的选点，不是事件队列。 */

typedef struct {
    int valid;
    int x;
    int y;
    unsigned long sequence;
} TouchSelection;

typedef struct {
    int pressed;
    int release_pending;
    int have_x;
    int have_y;
    int raw_x;
    int raw_y;
    unsigned long sequence;
    DisplayTransform transform;
} TouchParser;

typedef struct {
    int fd;
    atomic_int running;
    pthread_mutex_t lock;
    TouchParser parser;
    TouchSelection pending;
} TouchContext;

/* 输入 transform，输出 parser 的初始状态和变换副本；无返回值，空指针无操作。 */
void touch_parser_init(TouchParser *parser, const DisplayTransform *transform);

/* 输入 parser、单个 event，输出 selection；返回 1 有效释放选点、0 未形成选点、
 * -1 参数错误。解析器由调用线程独占；不读取设备、不等待事件。 */
int touch_process_event(TouchParser *parser, const struct input_event *event,
                        TouchSelection *selection);

/* 输入 device 路径和显示变换，输出 context；返回 0/-1。
 * 只打开设备并初始化，不创建线程；成功后须 stop/join/close。 */
int touch_init(TouchContext *context, const char *device,
               const DisplayTransform *transform);

/* argument 为 TouchContext*；轮询设备并写 pending，退出返回 NULL。
 * 应由 pthread_create 调用；设备错误或 stop 请求终止循环。 */
void *touch_run(void *argument);

/* 输入 context；原子设置停止标志，无返回值、不等待线程退出，随后须 join。 */
void touch_stop(TouchContext *context);

/* 输入 context，输出 selection；返回 1 已取走选点、0 无新选点、-1 参数错误。
 * 持锁复制后清除 pending；返回 0 时不更新输出，调用方不得使用旧输出。 */
int touch_take_selection(TouchContext *context, TouchSelection *selection);

/* 输入成功初始化且线程已 join 的 context；关闭 fd、销毁锁，无返回值。 */
void touch_close(TouchContext *context);

#endif
