/* 公开接口输入输出见 gimbal.h。图像线程短暂持锁更新目标；控制线程独占 fd。
 * minimum/maximum/polarity 在启动前配置，运行期间只读。
 * transmit 输入地址/数据/长度<=8，返回 0/-1（本地发送结果，并非执行确认）。
 * receive_frame 输入地址/功能码，输出匹配帧，返回 0/-1，总窗口 REPLY_MS。
 * read_position 输入地址，输出反馈电机轴度数，返回 0/-1，失败时输出不可用。
 * enable_motor 输入地址，返回 0/-1，必须收到接收确认才成功。
 * set_speed 输入地址和有符号 RPM，返回 0/-1；0 使用停止指令。
 * drain 无输出，丢弃积压帧；stop_motors 无返回值，尝试停车并报告发送失败。
 * control_thread 输入未使用、返回 NULL；通信故障锁定到重启，不自动恢复。
 * 本模块不配置波特率，不提供硬件急停；故障时不能保证停车送达。
 */
#define _POSIX_C_SOURCE 200809L
#include "gimbal.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define PERIOD_MS 50
#define STALE_MS 250
#define REPLY_MS 100
#define MAX_RPM 3
#define DEADZONE 0.04
#define LIMIT_MARGIN 2.0
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int active, quit, target_valid, fd = -1;
static double error_xy[2], minimum[2], maximum[2];
static int polarity[2];
static uint64_t target_ms;

/* 返回单调毫秒时间，用于帧新鲜度和回复总超时判断。 */
static uint64_t milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return 0;
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* 中心半径 4% 死区；其余偏差映射为 1~3 RPM，靠近边界仅允许向内。 */
int gimbal_axis_speed(double error, double position, double low, double high, int direction)
{
    double magnitude;
    int rpm;
    if (!isfinite(error) || !isfinite(position) || !isfinite(low) || !isfinite(high) ||
        low >= high || (direction != 1 && direction != -1)) return 0;
    magnitude = error < 0 ? -error : error;
    if (magnitude <= DEADZONE) return 0;
    rpm = magnitude >= 1.0 ? MAX_RPM : 1 + (int)(magnitude * (MAX_RPM - 1));
    if (error < 0) rpm = -rpm;
    rpm *= direction;
    if ((rpm > 0 && position >= high - LIMIT_MARGIN) ||
        (rpm < 0 && position <= low + LIMIT_MARGIN)) return 0;
    return rpm;
}

/* 持久非阻塞 SocketCAN 发送扩展帧，写队列满时立即报错。 */
static int transmit(int address, const unsigned char *data, size_t length)
{
    struct can_frame f = {0};
    ssize_t n;
    f.can_id = CAN_EFF_FLAG | ((canid_t)address << 8);
    f.can_dlc = (unsigned char)length;
    memcpy(f.data, data, length);
    do { n = write(fd, &f, sizeof(f)); } while (n < 0 && errno == EINTR);
    return n == sizeof(f) ? 0 : -1;
}

/* 清除上一轮回复，防止把积压的位置数据当作当前测量。 */
static void drain(void)
{
    struct can_frame f;
    for (int i = 0; i < 128; ++i)
        if (read(fd, &f, sizeof(f)) != sizeof(f)) break;
}

/* 在固定总窗口中等待指定地址/功能码，其他帧不延长超时。 */
static int receive_frame(int address, int code, struct can_frame *f)
{
    uint64_t end = milliseconds() + REPLY_MS;
    for (;;) {
        uint64_t now = milliseconds();
        struct pollfd p = {fd, POLLIN, 0};
        int n;
        if (now >= end) return -1;
        n = poll(&p, 1, (int)(end - now));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || !(p.revents & POLLIN)) return -1;
        if (read(fd, f, sizeof(*f)) != sizeof(*f)) continue;
        if (f->can_id == (CAN_EFF_FLAG | ((canid_t)address << 8)) &&
            f->can_dlc >= 2 && f->can_dlc <= 8 &&
            f->data[0] == code && f->data[f->can_dlc - 1] == 0x6B) return 0;
    }
}

/* 读取电机编码器角度，单位为电机轴度数，与 position 命令一致。 */
static int read_position(int address, double *position)
{
    const unsigned char data[] = {0x36, 0x6B};
    struct can_frame f;
    uint32_t raw;
    drain();
    if (transmit(address, data, sizeof(data)) < 0 || receive_frame(address, 0x36, &f) < 0 ||
        f.can_dlc != 7 || f.data[1] > 1) return -1;
    raw = ((uint32_t)f.data[2] << 24) | ((uint32_t)f.data[3] << 16) |
          ((uint32_t)f.data[4] << 8) | f.data[5];
    *position = raw * (360.0 / 65536.0) * (f.data[1] ? -1 : 1);
    return 0;
}

/* 使能后必须获得接收确认，失败时不进入运动控制。 */
static int enable_motor(int address)
{
    const unsigned char data[] = {0xF3, 0xAB, 1, 0, 0x6B};
    struct can_frame f;
    drain();
    return transmit(address, data, sizeof(data)) == 0 &&
           receive_frame(address, 0xF3, &f) == 0 && f.can_dlc == 3 && f.data[1] == 2 ? 0 : -1;
}

/* 零速度使用立即停止指令，非零速度使用 Emm F6，固定较缓加速度 5。 */
static int set_speed(int address, int rpm)
{
    unsigned char stop[] = {0xFE, 0x98, 0, 0x6B};
    unsigned char data[] = {0xF6, rpm < 0, 0, (unsigned char)(rpm < 0 ? -rpm : rpm), 5, 0, 0x6B};
    return rpm == 0 ? transmit(address, stop, sizeof(stop)) : transmit(address, data, sizeof(data));
}

/* 尝试停止两轴；总线故障时无法保证送达，因此明确输出错误。 */
static void stop_motors(void)
{
    int a = set_speed(2, 0), b = set_speed(3, 0);
    if (a < 0 || b < 0) fprintf(stderr, "Gimbal stop send failed; check motors physically\n");
}

/* 控制线程持有 CAN：定期读角度，处理最新目标，并在通信故障后锁定停车。 */
static void *control_thread(void *unused)
{
    int enabled = 0, fault = 0;
    (void)unused;
    for (;;) {
        uint64_t begin = milliseconds(), stamp;
        double e[2], pos[2];
        int valid, exiting;
        pthread_mutex_lock(&lock);
        exiting = quit; valid = target_valid; stamp = target_ms;
        e[0] = error_xy[0]; e[1] = error_xy[1];
        pthread_mutex_unlock(&lock);
        if (exiting) break;
        if (fault || !valid || begin < stamp || begin - stamp > STALE_MS) {
            stop_motors();
        } else {
            if (!enabled) {
                if (enable_motor(2) < 0 || enable_motor(3) < 0) fault = 1;
                else enabled = 1;
            }
            if (!fault && (read_position(2, &pos[0]) < 0 || read_position(3, &pos[1]) < 0)) fault = 1;
            /* CAN 等待期间目标可能丢失或退出，发送前再次确认。 */
            pthread_mutex_lock(&lock);
            valid = target_valid && !quit && milliseconds() >= target_ms &&
                    milliseconds() - target_ms <= STALE_MS;
            e[0] = error_xy[0]; e[1] = error_xy[1];
            pthread_mutex_unlock(&lock);
            if (!fault && valid) {
                for (int i = 0; i < 2; ++i) {
                    if (pos[i] < minimum[i] || pos[i] > maximum[i]) { fault = 1; break; }
                }
                if (!fault) {
                    int a = set_speed(2, gimbal_axis_speed(e[0], pos[0], minimum[0], maximum[0], polarity[0]));
                    int b = set_speed(3, gimbal_axis_speed(e[1], pos[1], minimum[1], maximum[1], polarity[1]));
                    if (a < 0 || b < 0) fault = 1;
                }
            }
            if (fault || !valid) stop_motors();
            if (fault) fprintf(stderr, "Gimbal fault: CAN/position/limits; motion locked until restart\n");
        }
        {
            uint64_t elapsed = milliseconds() - begin;
            struct timespec delay = {0, elapsed < PERIOD_MS ? (long)(PERIOD_MS - elapsed) * 1000000L : 1000000L};
            nanosleep(&delay, NULL);
        }
    }
    stop_motors();
    return NULL;
}

/* 读取显式边界及方向配置，验证当前角度后启动线程；不擅自把上电位置当中心。 */
int gimbal_start(void)
{
    const char *config = getenv("GIMBAL_CONFIG");
    char interface[IF_NAMESIZE], tail;
    struct sockaddr_can bus = {0};
    struct can_filter filters[2];
    double pos;
    unsigned index;
    if (active) return -1;
    if (!config || !*config) return 0;
    if (sscanf(config, "%15s %lf %lf %lf %lf %d %d %c", interface,
               &minimum[0], &maximum[0], &minimum[1], &maximum[1], &polarity[0], &polarity[1], &tail) != 7)
        goto bad_config;
    for (int i = 0; i < 2; ++i)
        if (!isfinite(minimum[i]) || !isfinite(maximum[i]) || maximum[i] - minimum[i] <= 4 ||
            (polarity[i] != 1 && polarity[i] != -1)) goto bad_config;
    index = if_nametoindex(interface);
    if (!index) return -1;
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto fail;
    for (int i = 0; i < 2; ++i) {
        filters[i].can_id = CAN_EFF_FLAG | ((canid_t)(i + 2) << 8);
        /* mask不能包含CAN_ERR_FLAG，否则进入内核错误帧接收列表。 */
        filters[i].can_mask = CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    }
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters)) < 0) goto fail;
    bus.can_family = AF_CAN; bus.can_ifindex = (int)index;
    if (bind(fd, (struct sockaddr *)&bus, sizeof(bus)) < 0) goto fail;
    stop_motors();
    for (int i = 0; i < 2; ++i)
        if (read_position(i + 2, &pos) < 0 || pos < minimum[i] || pos > maximum[i]) goto fail;
    quit = target_valid = 0;
    if (pthread_create(&worker, NULL, control_thread, NULL) != 0) goto fail;
    active = 1;
    puts("Gimbal enabled: base=2 tilt=3; select target to follow");
    return 0;
bad_config:
    fprintf(stderr, "GIMBAL_CONFIG: can0 base_min base_max tilt_min tilt_max x_sign y_sign\n");
    return -1;
fail:
    stop_motors(); close(fd); fd = -1;
    return -1;
}

/* 图像线程仅提交最新结果，不等待电机收发，原有串口日志照常输出。 */
void gimbal_update(int x, int y, int width, int height, int valid, uint64_t frame_ms)
{
    pthread_mutex_lock(&lock);
    target_valid = valid && frame_ms != 0 && width > 1 && height > 1 && x >= 0 && y >= 0 && x < width && y < height;
    if (target_valid) {
        error_xy[0] = (x - (width - 1) / 2.0) / (width / 2.0);
        error_xy[1] = (y - (height - 1) / 2.0) / (height / 2.0);
    }
    target_ms = frame_ms;
    pthread_mutex_unlock(&lock);
}

/* 通知线程退出并等待停车尝试完成；静态互斥锁保留以容纳最后的图像更新。 */
void gimbal_stop(void)
{
    if (!active) return;
    pthread_mutex_lock(&lock); quit = 1; target_valid = 0; pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    close(fd); fd = -1; active = 0;
}
