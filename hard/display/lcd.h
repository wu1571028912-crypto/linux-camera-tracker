#ifndef LCD_H
#define LCD_H

#include <stdint.h>
/* 单实例：init -> show_frame/get_size -> close；只由一个显示线程调用。
 * init 无输入，返回 0/-1，映射 /dev/fb0，当前要求 RGB565 16bpp。
 * show_frame 输入只读 RGB565、像素宽高和像素步长，输出写 framebuffer，
 * 无返回值、不保留源指针；按比例居中黑边显示，须先 init 成功。
 * get_size 向非空指针输出像素尺寸；close 无返回值，必须先结束显示线程。
 */

/* 初始化并映射 16 bpp 的 /dev/fb0，成功返回 0，失败返回 -1。 */
int lcd_init(void);

/* 将一帧 RGB565 图像按比例缩放并显示到 LCD。 */
void lcd_show_frame(const uint16_t *rgb565,
                    int source_width, int source_height, int source_stride);

/* 获取 LCD 的可见宽度和高度，输出参数允许为 NULL。 */
void lcd_get_size(int *width, int *height);

/* 解除 framebuffer 映射并释放 LCD 模块资源。 */
void lcd_close(void);

#endif
