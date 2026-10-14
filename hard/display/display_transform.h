#ifndef DISPLAY_TRANSFORM_H
#define DISPLAY_TRANSFORM_H

#include <stdint.h>
/* 纯坐标/图像函数，无硬件资源；尺寸、坐标和 stride 均以像素为单位。
 * 原图/LCD 左上为原点，右下为正；不做旋转、镜像或触摸原始量程标定。
 * init 输入源/LCD 正尺寸，输出 transform，返回 0/-1。
 * lcd_to_source 输入 LCD 点，输出原图点，返回 0/-1；失败不得使用输出。
 * source_to_lcd 输入原图点（越界钳位），输出 LCD 点，无返回值。
 * box_from_center 输入图像尺寸、框边长、中心，输出闭区间四边界；无返回值。
 * render 输入只读 source 和匹配源/目标尺寸的 transform，输出 destination；
 * 无返回值，目标至少 height*stride 个 uint16_t，源目标不可重叠。
 * 所有输出缓冲区均由调用者分配；void 函数非法参数时直接返回、不保证更新输出。
 */

typedef struct {
    int source_width;
    int source_height;
    int lcd_width;
    int lcd_height;
    int render_width;
    int render_height;
    int offset_x;
    int offset_y;
} DisplayTransform;

/* 计算源图像在 LCD 上保持宽高比的显示区域。 */
int display_transform_init(DisplayTransform *transform,
                           int source_width, int source_height,
                           int lcd_width, int lcd_height);

/* 将 LCD 坐标换算为源图像坐标；触点在黑边时返回 -1。 */
int display_lcd_to_source(const DisplayTransform *transform,
                          int lcd_x, int lcd_y,
                          int *source_x, int *source_y);

/* 将源图像坐标换算为 LCD 坐标。 */
void display_source_to_lcd(const DisplayTransform *transform,
                           int source_x, int source_y,
                           int *lcd_x, int *lcd_y);

/* 生成以指定点为中心且不越界的方框坐标。 */
void display_box_from_center(int width, int height, int box_size,
                             int center_x, int center_y,
                             int *x1, int *y1, int *x2, int *y2);

/* 最近邻缩放 RGB565 图像，并用黑色填充未覆盖的 LCD 区域。 */
void display_render_letterbox_rgb565(uint16_t *destination,
                                     int destination_width,
                                     int destination_height,
                                     int destination_stride,
                                     const uint16_t *source,
                                     int source_width,
                                     int source_height,
                                     int source_stride,
                                     const DisplayTransform *transform);

#endif
