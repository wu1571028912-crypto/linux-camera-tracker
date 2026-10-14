#include "gimbal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
/* 验证方向、中心死区、速度饱和及两端允许返回的软限位行为。 */
int main(void)
{
    assert(gimbal_axis_speed(0, 0, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(0.04, 0, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(-0.03, 0, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(0.2, 0, -90, 90, 1) == 1);
    assert(gimbal_axis_speed(0.2, 0, -90, 90, -1) == -1);
    assert(gimbal_axis_speed(-0.8, 0, -90, 90, 1) == -2);
    assert(gimbal_axis_speed(1e300, 0, -90, 90, 1) == 3);
    assert(gimbal_axis_speed(1, 88, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(-1, 88, -90, 90, 1) == -3);
    assert(gimbal_axis_speed(-1, -88, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(1, -88, -90, 90, 1) == 3);
    assert(gimbal_axis_speed(1, -88, -90, 90, -1) == 0);
    assert(gimbal_axis_speed(NAN, 0, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(1, NAN, -90, 90, 1) == 0);
    assert(gimbal_axis_speed(1, 0, 90, -90, 1) == 0);
    assert(gimbal_axis_speed(1, 0, -90, 90, 0) == 0);
    puts("gimbal control tests passed");
    return 0;
}
