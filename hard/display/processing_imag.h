#ifndef PROCESSING_IMAG_H
#define PROCESSING_IMAG_H

#include <stdint.h>

#include "tracker.h"

/* 输入/输出 image：可写 RGB565，容量至少 height*stride 个 uint16_t。
 * width/height/stride 单位像素，result 使用原图坐标；无返回值。
 * 原地画 TRACKING 绿框、LOST_SEARCH 红框、白色中心十字，IDLE 不绘制。
 * 不分配/释放图像；须识别完成后绘制，避免框污染跟踪模板。 */
void draw_tracking_overlay_rgb565(uint16_t *image,
                                  int width, int height, int stride,
                                  const TrackerResult *result);

#endif
