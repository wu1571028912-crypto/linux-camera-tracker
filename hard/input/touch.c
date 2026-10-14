/* 事件解析/线程/选点接口契约见 touch.h。parser 由读取线程独占，pending 受锁保护。
 * raw_x/y 直接按 LCD 像素解释，更换触摸屏须检查坐标量程和事件类型。
 */
#include "touch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 清空触摸解析器状态，并保存 LCD 与源图像之间的坐标变换。 */
void touch_parser_init(TouchParser *parser, const DisplayTransform *transform)
{
    if (parser == NULL || transform == NULL)
        return;
    memset(parser, 0, sizeof(*parser));
    parser->transform = *transform;
}

/*
 * 将单个 evdev 事件送入触摸状态机。
 * 完整的按下、ABS 坐标、释放和 SYN_REPORT 序列产生一次选择并返回 1；
 * 尚未形成选择时返回 0，参数非法时返回 -1。
 */
int touch_process_event(TouchParser *parser, const struct input_event *event,
                        TouchSelection *selection)
{
    if (parser == NULL || event == NULL || selection == NULL)
        return -1;

    selection->valid = 0;

    if (event->type == EV_KEY && event->code == BTN_TOUCH) {
        if (event->value != 0) {
            parser->pressed = 1;
            parser->release_pending = 0;
            parser->have_x = 0;
            parser->have_y = 0;
        } else if (parser->pressed) {
            parser->pressed = 0;
            parser->release_pending = 1;
        }
    } else if (event->type == EV_ABS && event->code == ABS_X) {
        parser->raw_x = event->value;
        parser->have_x = 1;
    } else if (event->type == EV_ABS && event->code == ABS_Y) {
        parser->raw_y = event->value;
        parser->have_y = 1;
    } else if (event->type == EV_SYN && event->code == SYN_REPORT &&
               parser->release_pending) {
        if (parser->have_x && parser->have_y &&
            display_lcd_to_source(&parser->transform,
                                  parser->raw_x, parser->raw_y,
                                  &selection->x, &selection->y) == 0) {
            parser->sequence++;
            selection->valid = 1;
            selection->sequence = parser->sequence;
        }
        parser->release_pending = 0;
        parser->have_x = 0;
        parser->have_y = 0;
    }

    return selection->valid ? 1 : 0;
}

/*
 * 初始化线程同步对象并以非阻塞方式打开触摸设备。
 * 成功返回 0；失败返回 -1，并销毁已经创建的互斥锁。
 */
int touch_init(TouchContext *context, const char *device,
               const DisplayTransform *transform)
{
    if (context == NULL || device == NULL || transform == NULL)
        return -1;

    memset(context, 0, sizeof(*context));
    context->fd = -1;
    if (pthread_mutex_init(&context->lock, NULL) != 0)
        return -1;

    context->fd = open(device, O_RDONLY | O_NONBLOCK);
    if (context->fd < 0) {
        fprintf(stderr, "open touch device %s failed: %s\n",
                device, strerror(errno));
        pthread_mutex_destroy(&context->lock);
        return -1;
    }

    touch_parser_init(&context->parser, transform);
    atomic_store(&context->running, 1);
    return 0;
}

/*
 * 触摸读取线程入口，每 100 ms 轮询一次 evdev 设备。
 * 新选择点在互斥锁保护下写入 pending，收到停止请求或设备错误时退出。
 */
void *touch_run(void *argument)
{
    TouchContext *context = argument;
    struct pollfd descriptor;

    if (context == NULL)
        return NULL;

    descriptor.fd = context->fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    while (atomic_load(&context->running)) {
        int poll_result = poll(&descriptor, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll touch device failed: %s\n", strerror(errno));
            break;
        }
        if (poll_result == 0)
            continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "touch device became unavailable (revents=0x%x)\n",
                    descriptor.revents);
            break;
        }
        if (descriptor.revents & POLLIN) {
            struct input_event event;
            ssize_t bytes = read(context->fd, &event, sizeof(event));
            TouchSelection selection = {0};

            if (bytes < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                fprintf(stderr, "read touch device failed: %s\n", strerror(errno));
                break;
            }
            if (bytes != (ssize_t)sizeof(event)) {
                fprintf(stderr, "short touch event read: %zd bytes\n", bytes);
                break;
            }

            touch_process_event(&context->parser, &event, &selection);
            if (selection.valid) {
                pthread_mutex_lock(&context->lock);
                context->pending = selection;
                pthread_mutex_unlock(&context->lock);
            }
        }
    }

    atomic_store(&context->running, 0);
    return NULL;
}

/* 原子清除运行标志，使触摸读取线程在下一次轮询后退出。 */
void touch_stop(TouchContext *context)
{
    if (context != NULL)
        atomic_store(&context->running, 0);
}

/*
 * 取走一次尚未消费的触摸选择点。
 * 有数据时复制到 selection、清除 pending 并返回 1；无数据返回 0。
 */
int touch_take_selection(TouchContext *context, TouchSelection *selection)
{
    int available;

    if (context == NULL || selection == NULL)
        return -1;

    pthread_mutex_lock(&context->lock);
    available = context->pending.valid;
    if (available) {
        *selection = context->pending;
        context->pending.valid = 0;
    }
    pthread_mutex_unlock(&context->lock);
    return available ? 1 : 0;
}

/* 关闭触摸设备描述符并销毁互斥锁，应在触摸线程退出后调用。 */
void touch_close(TouchContext *context)
{
    if (context == NULL)
        return;
    if (context->fd >= 0) {
        close(context->fd);
        context->fd = -1;
    }
    pthread_mutex_destroy(&context->lock);
}
