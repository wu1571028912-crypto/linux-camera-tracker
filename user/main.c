/* 入口输入：./make /dev/video0 [触摸设备]，以及可选 GIMBAL_CONFIG 环境变量。
 * 输出：LCD 图像、stdout 跟踪日志、stderr 错误、启用后的 CAN 指令。
 * 数据流：采集 -> raw_queue -> processing_thread -> processed_queue -> display_thread。
 * touch_run 提交选点，processing_thread 向云台提交原图中心。
 * processing_thread 的 argument 是 ProcessingContext*，消费/释放输入帧副本，
 * 输出绘制后的帧副本和坐标；返回 NULL。display_thread 输入 FrameQueue*，
 * 输出到 LCD 并释放每个出队帧，返回 NULL。monotonic_ms 无输入，返回毫秒/失败0。
 * main 返回 EXIT_SUCCESS 表示捕获退出请求，初始化/采集故障返回 EXIT_FAILURE。
 * 资源生命周期：设备初始化 -> 启动线程 -> 采集 -> 停车/关队列 -> join -> 释放。
 */
#define _POSIX_C_SOURCE 200809L // 启用 POSIX 2008 标准，确保可跨平台使用对应的标准库函数

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "camera.h"
#include "display_transform.h"
#include "frame_queue.h"
#include "lcd.h"
#include "processing_imag.h"
#include "touch.h"
#include "tracker.h"
#include "tracking_log.h"
#include "gimbal.h"

// 图像处理线程的上下文结构体，用于在不同线程间传递核心参数
typedef struct {
    FrameQueue *input;      // 指向原始图像队列（从摄像头来）
    FrameQueue *output;     // 指向处理后图像队列（准备发给LCD）
    TouchContext *touch;    // 指向触摸屏上下文，用于获取用户点击坐标
    int frame_width;        // 图像宽度
    int frame_height;       // 图像高度
} ProcessingContext;

static volatile sig_atomic_t running = 1; // 线程安全标志位，1表示运行，0表示准备退出
static FrameQueue raw_queue;              // 原始图像队列，主线程填入，处理线程消耗
static FrameQueue processed_queue;        // 处理后的图像队列，处理线程填入，显示线程消耗

/* 
 * 信号处理函数：捕获 Ctrl+C (SIGINT)。
 * 仅修改 running 标志位为0，通知主循环和线程准备退出，不由信号处理函数直接做清理。
 */
static void sigint_handler(int signal_number)
{
    (void)signal_number;
    running = 0;
}

/* 
 * 获取系统的单调时间（毫秒级），不受系统时间被手动修改的影响。
 * 主要用于控制日志打印频率（节流）。
 */
static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * 图像处理线程的核心函数
 * 流程：从输入队列拿图像 -> 读取屏幕触摸坐标 -> 锁定/更新跟踪目标 -> 画上跟踪框 -> 推入输出队列
 */
static void *processing_thread(void *argument)
{
    ProcessingContext *context = argument;
    Tracker tracker;              // 持续工作的跟踪器
    TrackerResult previous;       // 上一次处理结果
    uint64_t last_emit_ms = 0;
    Frame frame;                  // 当前正在处理的图像

    memset(&previous, 0, sizeof(previous));
    previous.state = TRACKER_IDLE;
    
    // 初始化图像跟踪算法
    if (tracker_init(&tracker, context->frame_width, context->frame_height) < 0) {
        fprintf(stderr, "Tracker init failed\n");
        frame_queue_close(context->output);
        return NULL;
    }

    // 不断从 raw_queue 提取摄像头画面
    while (frame_queue_pop(context->input, &frame) == 0) {
        TouchSelection selection;   // 用户最新一次触摸选点
        TrackerResult current;      // 当前这一次处理结果
        char line[192];
        
        // 检查有没有新的触摸选点，如果有则尝试锁定目标
        int selected = touch_take_selection(context->touch, &selection);
        
        if (selected > 0) {
            // 用户点击了屏幕，初始化跟踪器并锁定点击位置的目标
            if (tracker_select(&tracker, (const uint16_t *)frame.data,
                               frame.width, frame.height, frame.stride,
                               selection.x, selection.y) < 0) {
                fprintf(stderr, "Cannot select target at (%d, %d)\n",
                        selection.x, selection.y);
            }
            tracker_get_result(&tracker, &current);
        } else {
            // 用户没点击，继续更新跟踪上一帧锁定的目标位置
            if (tracker_update(&tracker, (const uint16_t *)frame.data,
                               frame.width, frame.height, frame.stride,
                               &current) < 0) {
                tracker_get_result(&tracker, &current);
            }
        }
        
        // 格式化并打印当前的跟踪状态日志（附带节流控制）
        if (tracking_format_line(line, sizeof(line), &current, &previous,
                                 monotonic_ms(), &last_emit_ms) > 0) {
            puts(line);
            fflush(stdout);
        }
        previous = current;
        /* 原图中心坐标直接提交给云台线程，日志与显示保持独立。 */
        // gimbal_update(current.center_x, current.center_y, frame.width, frame.height,
        //               running && current.state == TRACKER_TRACKING, frame.queued_ms);

        // 原地绘制状态框：跟踪绿色，丢失搜索红色。
        draw_tracking_overlay_rgb565((uint16_t *)frame.data,
                                     frame.width, frame.height, frame.stride,
                                     &current);
                                     
        // 将画好框的图像推入 processed_queue，交由显示线程处理
        if (frame_queue_push(context->output, frame.data,
                             frame.width, frame.height, frame.stride) < 0 &&
            running) {
            fprintf(stderr, "Display queue full, frame dropped\n");
        }
        
        // 释放当前这帧在处理线程中的内存占用
        free(frame.data);
    }

    // 线程退出前关闭输出队列，防止下级线程死锁等待
    gimbal_update(0, 0, 0, 0, 0, 0);
    frame_queue_close(context->output);
    return NULL;
}

/*
 * 屏幕显示线程
 * 流程：只负责从输出队列拿图像，然后刷到 LCD 屏幕上。
 */
static void *display_thread(void *argument)
{
    FrameQueue *queue = argument;
    Frame frame;

    // 不断从 processed_queue 提取处理好的画面
    while (frame_queue_pop(queue, &frame) == 0) {
        // 调用底层 LCD 驱动接口显示画面
        lcd_show_frame((const uint16_t *)frame.data,
                       frame.width, frame.height, frame.stride);
        free(frame.data); // 显示完毕，释放内存
    }
    return NULL;
}

/*
 * 主函数：负责全部硬件的初始化，统筹各条线程，并在最后优雅地清理资源。
 */
int main(int argc, char *argv[])
{
    // 默认触摸设备节点
    const char *touch_device = "/dev/input/event1";
    DisplayTransform transform;
    TouchContext touch;
    ProcessingContext processing;
    pthread_t touch_thread;
    pthread_t process_thread;
    pthread_t screen_thread;
    
    // 变量声明
    int lcd_width, lcd_height;
    int frame_width, frame_height;
    int touch_started = 0, process_started = 0;
    int screen_started = 0, stream_started = 0;
    int queues_initialized = 0;
    int exit_status = EXIT_FAILURE;

    // 参数校验
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <video_dev> [touch_dev]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 3)
        touch_device = argv[2];

    // 注册 Ctrl+C 信号处理
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // ================= 硬件与参数初始化阶段 =================
    if (lcd_init() < 0) {
        fprintf(stderr, "LCD init failed\n");
        return EXIT_FAILURE;
    }
    if (camera_init(argv[1]) < 0) {
        fprintf(stderr, "Camera init failed\n");
        goto close_lcd; // 出错时跳转到指定标签，执行对应的资源释放清理
    }
    
    camera_enum_formats();
    //取得 LCD 可见显示区域的像素宽高。
    lcd_get_size(&lcd_width, &lcd_height);
    
    // 让摄像头的输出格式尽量匹配 LCD 的尺寸
    if (camera_set_format(lcd_width, lcd_height) < 0) {
        fprintf(stderr, "Set camera format failed\n");
        goto close_camera;
    }
    camera_get_framesize(&frame_width, &frame_height);
    
    // 初始化显示坐标变换（解决摄像头图像尺寸和LCD屏幕物理尺寸的适配/居中问题）
    if (display_transform_init(&transform, frame_width, frame_height,
                               lcd_width, lcd_height) < 0) {
        fprintf(stderr, "Display transform init failed\n");
        goto close_camera;
    }
    printf("Camera %dx%d -> LCD %dx%d, image area %dx%d at (%d,%d)\n",
           frame_width, frame_height, lcd_width, lcd_height,
           transform.render_width, transform.render_height,
           transform.offset_x, transform.offset_y);

    if (touch_init(&touch, touch_device, &transform) < 0)
        goto close_camera;
    
    // 分配 USERPTR 用户态缓冲区并交给 V4L2 驱动，不是 mmap。
    if (camera_init_buffers() < 0) {
        fprintf(stderr, "Camera buffer init failed\n");
        goto close_touch;
    }
    // 启动视频流采集
    if (camera_start_stream() < 0) {
        fprintf(stderr, "Start camera stream failed\n");
        goto close_touch;
    }
    stream_started = 1;

    // 初始化两个数据队列
    frame_queue_init(&raw_queue);
    frame_queue_init(&processed_queue);
    queues_initialized = 1;

    // 配置传给处理线程的上下文
    processing.input = &raw_queue;
    processing.output = &processed_queue;
    processing.touch = &touch;
    processing.frame_width = frame_width;
    processing.frame_height = frame_height;

    /* 仅在显式给出方向与软限位时开启电机自动跟踪。 */
    if (gimbal_start() < 0) {
        fprintf(stderr, "Gimbal startup failed: check CAN, bounds and Response setting\n");
        goto stop_threads;
    }

    // ================= 多线程启动阶段 =================
    if (pthread_create(&touch_thread, NULL, touch_run, &touch) != 0) {
        fprintf(stderr, "Create touch thread failed\n");
        goto stop_threads;
    }
    touch_started = 1;
    
    if (pthread_create(&process_thread, NULL, processing_thread, &processing) != 0) {
        fprintf(stderr, "Create processing thread failed\n");
        goto stop_threads;
    }
    process_started = 1;
    
    if (pthread_create(&screen_thread, NULL, display_thread, &processed_queue) != 0) {
        fprintf(stderr, "Create display thread failed\n");
        goto stop_threads;
    }
    screen_started = 1;

    // ================= 主循环：视频采集 =================
    puts("Tap the displayed image to select an 80x80 target; Ctrl+C exits.");
    while (running) {
        void *data;
        size_t size;

        // 从 V4L2 驱动提取一帧摄像头数据
        if (camera_get_frame(&data, &size) < 0) {
            if (running)
                fprintf(stderr, "Get frame failed\n");
            break;
        }
        (void)size;
        
        // 复制画面数据并推送到 raw_queue 供处理线程拿取
        frame_queue_push(&raw_queue, data, frame_width, frame_height, frame_width);
        
        // 将 V4L2 缓冲区交还给驱动，以便抓取下一帧
        if (camera_put_frame() < 0) {
            if (running)
                fprintf(stderr, "Return camera frame failed\n");
            break;
        }
    }
    // 如果是正常退出（比如按了Ctrl+C使得running变为0），标志程序成功
    if (!running)
        exit_status = EXIT_SUCCESS;

// ================= 资源回收阶段 =================
// 采用典型的内核倒序清理法（goto chain），确保无论上面哪一步失败跳到这里，都能正确清理已分配的资源。
stop_threads:
    running = 0;              // 通知所有线程准备退出
    gimbal_stop();
    touch_stop(&touch);
    
    if (queues_initialized) {
        frame_queue_close(&raw_queue);
        // 如果处理线程没启动，这里手动关闭输出队列解除显示线程阻塞
        if (!process_started) 
            frame_queue_close(&processed_queue);
    }
    
    // 阻塞等待各子线程安全退出
    if (touch_started) pthread_join(touch_thread, NULL);
    if (process_started) pthread_join(process_thread, NULL);
    if (screen_started) pthread_join(screen_thread, NULL);
    
    // 销毁队列内存
    if (queues_initialized) {
        frame_queue_destroy(&raw_queue);
        frame_queue_destroy(&processed_queue);
    }
    
    if (stream_started) camera_stop_stream();
    
close_touch:
    touch_close(&touch);
close_camera:
    camera_close();
close_lcd:
    lcd_close();
    
    return exit_status;
}
