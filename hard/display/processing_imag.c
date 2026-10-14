/* 图像叠加层，不参与定位；输入输出详见 processing_imag.h。
 * 内部 clamp_int 输入值和闭区间，返回合法画线位置，无副作用。
 */
#include "processing_imag.h"

#include <stddef.h>

#define RGB565_GREEN 0x07e0U
#define RGB565_RED   0xf800U
#define RGB565_WHITE 0xffffU

/* 将整数限制在闭区间 [minimum, maximum] 内。 */
static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

/*
 * 在传入的 RGB565 图像上原地绘制跟踪框和中心十字。
 * 正常跟踪使用绿色，丢失搜索使用红色；空闲或参数非法时不修改图像。
 */
void draw_tracking_overlay_rgb565(uint16_t *image,
                                  int width, int height, int stride,
                                  const TrackerResult *result)
{
    uint16_t color;
    int x1;
    int y1;
    int x2;
    int y2;
    int thickness;

    if (image == NULL || result == NULL || width <= 0 || height <= 0 ||
        stride < width || result->state == TRACKER_IDLE)
        return;

    color = result->state == TRACKER_TRACKING ? RGB565_GREEN : RGB565_RED;
    x1 = clamp_int(result->x1, 0, width - 1);
    y1 = clamp_int(result->y1, 0, height - 1);
    x2 = clamp_int(result->x2, 0, width - 1);
    y2 = clamp_int(result->y2, 0, height - 1);
    if (x1 > x2 || y1 > y2)
        return;

    for (thickness = 0; thickness < 2; ++thickness) {
        int top = clamp_int(y1 + thickness, 0, height - 1);
        int bottom = clamp_int(y2 - thickness, 0, height - 1);
        int left = clamp_int(x1 + thickness, 0, width - 1);
        int right = clamp_int(x2 - thickness, 0, width - 1);
        int x;
        int y;

        for (x = left; x <= right; ++x) {
            image[top * stride + x] = color;
            image[bottom * stride + x] = color;
        }
        for (y = top; y <= bottom; ++y) {
            image[y * stride + left] = color;
            image[y * stride + right] = color;
        }
    }

    if (result->center_x >= 0 && result->center_x < width &&
        result->center_y >= 0 && result->center_y < height) {
        int offset;
        for (offset = -4; offset <= 4; ++offset) {
            int x = result->center_x + offset;
            int y = result->center_y + offset;
            if (x >= 0 && x < width)
                image[result->center_y * stride + x] = RGB565_WHITE;
            if (y >= 0 && y < height)
                image[y * stride + result->center_x] = RGB565_WHITE;
        }
    }
}
