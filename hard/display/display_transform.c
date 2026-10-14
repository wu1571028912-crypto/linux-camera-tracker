/* 纯几何/缩放函数，输入输出和缓冲区大小要求见 display_transform.h。
 * 内部 clamp_int 输入整数/闭区间，返回钳位整数；整数缩放存在像素量化误差。
 */
#include "display_transform.h"

#include <stddef.h>

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
 * 根据源图像与 LCD 尺寸计算等比例显示区域和居中黑边偏移。
 * 参数非法时返回 -1，成功时写入 transform 并返回 0。
 */
int display_transform_init(DisplayTransform *transform,
                           int source_width, int source_height,
                           int lcd_width, int lcd_height)
{
    if (transform == NULL || source_width <= 0 || source_height <= 0 ||
        lcd_width <= 0 || lcd_height <= 0)
        return -1;

    transform->source_width = source_width;
    transform->source_height = source_height;
    transform->lcd_width = lcd_width;
    transform->lcd_height = lcd_height;

    if ((long long)lcd_width * source_height <=
        (long long)lcd_height * source_width) {
        transform->render_width = lcd_width;
        transform->render_height = source_height * lcd_width / source_width;
    } else {
        transform->render_height = lcd_height;
        transform->render_width = source_width * lcd_height / source_height;
    }

    if (transform->render_width <= 0 || transform->render_height <= 0)
        return -1;

    transform->offset_x = (lcd_width - transform->render_width) / 2;
    transform->offset_y = (lcd_height - transform->render_height) / 2;
    return 0;
}

/*
 * 将 LCD 触摸坐标换算为源图像坐标。
 * 触点位于黑边或参数非法时返回 -1，否则写入 source_x/source_y 并返回 0。
 */
int display_lcd_to_source(const DisplayTransform *transform,
                          int lcd_x, int lcd_y,
                          int *source_x, int *source_y)
{
    int relative_x;
    int relative_y;

    if (transform == NULL || source_x == NULL || source_y == NULL)
        return -1;
    if (lcd_x < transform->offset_x || lcd_y < transform->offset_y ||
        lcd_x >= transform->offset_x + transform->render_width ||
        lcd_y >= transform->offset_y + transform->render_height)
        return -1;

    relative_x = lcd_x - transform->offset_x;
    relative_y = lcd_y - transform->offset_y;
    *source_x = relative_x * transform->source_width / transform->render_width;
    *source_y = relative_y * transform->source_height / transform->render_height;
    *source_x = clamp_int(*source_x, 0, transform->source_width - 1);
    *source_y = clamp_int(*source_y, 0, transform->source_height - 1);
    return 0;
}

/* 将源图像坐标换算为 LCD 显示区域坐标，并将越界输入限制到图像范围。 */
void display_source_to_lcd(const DisplayTransform *transform,
                           int source_x, int source_y,
                           int *lcd_x, int *lcd_y)
{
    if (transform == NULL || lcd_x == NULL || lcd_y == NULL)
        return;

    source_x = clamp_int(source_x, 0, transform->source_width - 1);
    source_y = clamp_int(source_y, 0, transform->source_height - 1);
    *lcd_x = transform->offset_x +
             source_x * transform->render_width / transform->source_width;
    *lcd_y = transform->offset_y +
             source_y * transform->render_height / transform->source_height;
}

/*
 * 根据中心点生成不越过图像边界的方框坐标。
 * 方框大于图像时自动收缩，结果通过 x1、y1、x2、y2 返回。
 */
void display_box_from_center(int width, int height, int box_size,
                             int center_x, int center_y,
                             int *x1, int *y1, int *x2, int *y2)
{
    int left;
    int top;

    if (x1 == NULL || y1 == NULL || x2 == NULL || y2 == NULL ||
        width <= 0 || height <= 0 || box_size <= 0) {
        return;
    }

    if (box_size > width)
        box_size = width;
    if (box_size > height)
        box_size = height;

    left = center_x - box_size / 2;
    top = center_y - box_size / 2;
    left = clamp_int(left, 0, width - box_size);
    top = clamp_int(top, 0, height - box_size);

    *x1 = left;
    *y1 = top;
    *x2 = left + box_size - 1;
    *y2 = top + box_size - 1;
}

/*
 * 使用最近邻采样把 RGB565 源图像等比例缩放到目标缓冲区。
 * 调用会先把整个目标缓冲区清零，因此未被图像覆盖的区域显示为黑边。
 */
void display_render_letterbox_rgb565(uint16_t *destination,
                                     int destination_width,
                                     int destination_height,
                                     int destination_stride,
                                     const uint16_t *source,
                                     int source_width,
                                     int source_height,
                                     int source_stride,
                                     const DisplayTransform *transform)
{
    int y;

    if (destination == NULL || source == NULL || transform == NULL ||
        destination_width <= 0 || destination_height <= 0 ||
        destination_stride < destination_width || source_width <= 0 ||
        source_height <= 0 || source_stride < source_width)
        return;

    for (y = 0; y < destination_height; ++y) {
        int x;
        uint16_t *row = destination + y * destination_stride;
        for (x = 0; x < destination_width; ++x)
            row[x] = 0;
    }

    for (y = 0; y < transform->render_height; ++y) {
        int x;
        int source_y = y * source_height / transform->render_height;
        uint16_t *destination_row = destination +
            (transform->offset_y + y) * destination_stride + transform->offset_x;
        const uint16_t *source_row = source + source_y * source_stride;

        for (x = 0; x < transform->render_width; ++x) {
            int source_x = x * source_width / transform->render_width;
            destination_row[x] = source_row[source_x];
        }
    }
}
