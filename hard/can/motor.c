/* 公开接口见 motor.h。内部 now_ms 无输入，返回单调毫秒/失败-1。
 * send_packet 输入 fd/地址/包序号/数据及长度<=8，返回 0/-1，失败设 errno。
 * wait_ack 输入已按地址过滤的 fd 和功能码，返回 0 接收确认、-1 超时/拒绝/IO错误。
 * FD 指令拆成两帧，各帧保留功能码；扩展 ID 低字节为分包序号。
 * 接收确认不等于到位，超时也不代表未执行，禁止盲目重发相对运动。
 */
#define _POSIX_C_SOURCE 200809L
#include "motor.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>

/* 单调时钟不受系统时间调整影响，用于限制整个回复等待窗口。 */
static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* 发送经典 CAN 扩展数据帧，地址占 ID 高位，低字节为分包序号。 */
static int send_packet(int fd, uint8_t address, unsigned packet,
                       const uint8_t *data, unsigned length)
{
    struct can_frame frame = {0};
    ssize_t n;
    frame.can_id = CAN_EFF_FLAG | ((canid_t)address << 8) | packet;
    frame.can_dlc = (unsigned char)length;
    memcpy(frame.data, data, length);
    do { n = write(fd, &frame, sizeof(frame)); } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof(frame)) return 0;
    if (n >= 0) errno = EIO;
    return -1;
}

/* 等待对应功能码的接收成功回复；拒绝码与无回复都向上层报错。 */
static int wait_ack(int fd, uint8_t code)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        struct can_frame frame;
        int64_t current = now_ms();
        int64_t left;
        int result;
        ssize_t n;
        if (current < 0) return -1;
        left = MOTOR_REPLY_TIMEOUT_MS - (current - start);
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        result = poll(&p, 1, (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (result == 0) { errno = ETIMEDOUT; return -1; }
        if (!(p.revents & POLLIN)) { errno = EIO; return -1; }
        n = read(fd, &frame, sizeof(frame));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n != sizeof(frame)) { errno = EIO; return -1; }
        if (frame.can_dlc != 3 || frame.data[0] != code || frame.data[2] != 0x6B)
            continue;
        if (frame.data[1] == 0x02) return 0;
        /* 9F 是运动到位通知，不等价于本次命令接收确认。 */
        if (frame.data[1] == 0x9F) continue;
        errno = EPROTO;
        return -1;
    }
}

/* 按配置换算角度，并完成使能确认及两包 Emm FD 位置指令发送。 */
int motor_move_angle(uint8_t address, float angle_deg)
{
    struct sockaddr_can bus = {0};
    struct can_filter filter;
    uint8_t enable[] = {0xF3, 0xAB, 1, 0, 0x6B};
    uint8_t first[8] = {0xFD};
    uint8_t second[5] = {0xFD, 0, 2, 0, 0x6B};
    double magnitude, pulses_per_rev, rounded;
    uint32_t pulses;
    unsigned index;
    int fd, result = -1, saved_errno;
    if ((address != 2 && address != 3) || !isfinite(angle_deg)) {
        errno = EINVAL; return -1;
    }
    magnitude = angle_deg < 0 ? -(double)angle_deg : (double)angle_deg;
    if (magnitude > (address == 2 ? 180.0 : 90.0)) { errno = ERANGE; return -1; }
    if (magnitude == 0) return 0;
    pulses_per_rev = address == 2 ? MOTOR_BASE_PULSES_PER_REV : MOTOR_TILT_PULSES_PER_REV;
    rounded = magnitude * pulses_per_rev / 360.0 + 0.5;
    if (!isfinite(rounded) || pulses_per_rev <= 0 || rounded < 1 || rounded > UINT32_MAX ||
        MOTOR_SPEED_RPM == 0 || MOTOR_SPEED_RPM > 3000 || MOTOR_ACCELERATION > 255) {
        errno = ERANGE; return -1;
    }
    pulses = (uint32_t)rounded;
    index = if_nametoindex(MOTOR_CAN_INTERFACE);
    if (!index) return -1;
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return -1;
    filter.can_id = CAN_EFF_FLAG | ((canid_t)address << 8);
    /* 普通扩展帧过滤器不可加入CAN_ERR_FLAG；错误帧用CAN_RAW_ERR_FILTER另订阅。 */
    filter.can_mask = CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_EFF_MASK;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) goto done;
    bus.can_family = AF_CAN;
    bus.can_ifindex = (int)index;
    if (bind(fd, (struct sockaddr *)&bus, sizeof(bus)) < 0) goto done;
    if (send_packet(fd, address, 0, enable, sizeof(enable)) < 0 || wait_ack(fd, 0xF3) < 0)
        goto done;
    first[1] = angle_deg < 0;
    first[2] = MOTOR_SPEED_RPM >> 8;
    first[3] = MOTOR_SPEED_RPM & 0xFF;
    first[4] = MOTOR_ACCELERATION;
    first[5] = pulses >> 24;
    first[6] = pulses >> 16;
    first[7] = pulses >> 8;
    second[1] = pulses & 0xFF;
    if (send_packet(fd, address, 0, first, sizeof(first)) < 0 ||
        send_packet(fd, address, 1, second, sizeof(second)) < 0) goto done;
    result = wait_ack(fd, 0xFD);
done:
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}
