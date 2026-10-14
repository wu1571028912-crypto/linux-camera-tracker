#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>

/* 独立位置运动接口，并非视觉跟踪的 F6 速度接口；不可与 gimbal 并行控制。
 * 根据开发板和实际电机参数修改。CAN 接口须预先配置并启动。 */
#define MOTOR_CAN_INTERFACE "can0"
#define MOTOR_SPEED_RPM 5U
#define MOTOR_ACCELERATION 5U
/* 每个云台输出轴转一圈所需脉冲：360/步距角 * 细分 * 减速比。 */
#define MOTOR_BASE_PULSES_PER_REV 3200.0
#define MOTOR_TILT_PULSES_PER_REV 3200.0
#define MOTOR_REPLY_TIMEOUT_MS 1000

/*
 * 控制地址 2（底座）或 3（俯仰）相对当前实时位置运动。
 * angle_deg > 0 为 CW，< 0 为 CCW；单位为云台输出轴角度。
 * 内部打开 CAN、使能电机、发送分包位置命令并等待接收确认。
 * 返回 0：电机已确认接受命令，不表示运动完成；返回 -1：检查 errno。
 * 超时不代表电机未执行，不可直接重试，否则可能再次相对运动。
 * 调用方须串行调用，并保证目标在机械行程内；本接口没有回零及软限位。
 * 单次请求限制为底座 180°、俯仰 90°，不代表剩余安全行程。
 * 0°直接返回成功，不发送使能或运动命令。
 */
int motor_move_angle(uint8_t address, float angle_deg);

#endif
