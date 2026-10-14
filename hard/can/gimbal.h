#ifndef GIMBAL_H
#define GIMBAL_H
#include <stdint.h>
/* 视觉云台单实例模块：地址 2=底座、3=俯仰，内部线程独占 CAN 收发。
 * 生命周期 start -> 重复 update -> stop；不要与 motor_move_angle 并行控制。
 * 本模块使用电机反馈轴角度，不自动换算机械减速比、不自动回零。
 */
/* 输入：环境变量 GIMBAL_CONFIG，格式为
 * "can0 base_min base_max tilt_min tilt_max x_sign y_sign"。
 * 边界单位为反馈角度，方向为 +/-1；接口须预先配置并 UP。
 * 返回 0：启动成功，或者未设置环境变量而跳过启动；-1：启动失败。
 * 成功启动会打印 Gimbal enabled；启动会尝试停车并查询两轴位置。
 * 在创建图像线程前调用；具体标定要求见 GIMBAL.md。 */
int gimbal_start(void);
/* 输入 x/y：原图像素中心，原点左上，右/下为正；width/height：原图尺寸。
 * valid=0 表示无目标；frame_ms：CLOCK_MONOTONIC 毫秒入队时间，不能用墙钟。
 * 无返回值；复制最新坐标，短暂持有互斥锁，不等待 CAN，不保证立即运动。
 * 非法坐标/零时间视为无目标；超过 250ms 的数据由控制线程拒绝。 */
void gimbal_update(int x, int y, int width, int height, int valid, uint64_t frame_ms);
/* 无输入/返回值；阻塞等待控制线程尝试停车并关闭 CAN，未启动时无操作。
 * 可以先于图像线程退出调用；停止不等于失能，总线故障时停车不保证送达。 */
void gimbal_stop(void);
/* 纯计算，无硬件副作用。error 为按半幅归一化的偏差；position/low/high
 * 使用同一电机反馈角度坐标；direction=+1/-1 为误差到电机方向的映射。
 * 返回有符号整数 RPM（-3..3），0 表示死区/向外限位/非法输入。
 * 这是分档调速，不是精确线性比例或 PID；离边界 2 度内禁止向外。 */
int gimbal_axis_speed(double error, double position, double low, double high, int direction);
#endif
