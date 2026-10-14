/* 显示后端，参数与生命周期见 lcd.h。screen_base 是 mmap 地址，不可 free；
 * staging_buffer 是堆内存；lcd_stride 为像素，screen_size 为字节。
 */
#include "lcd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "display_transform.h"

#define FB_DEV "/dev/fb0"

static int fb_fd = -1;
static uint16_t *screen_base = NULL;
static uint16_t *staging_buffer = NULL;
static size_t screen_size = 0;
static int lcd_width;
static int lcd_height;
static int lcd_stride;

/*
 * 打开并映射 /dev/fb0，读取屏幕尺寸并分配中间显示缓冲区。
 * 仅支持 16 bpp framebuffer；成功返回 0，失败返回 -1 并回收已创建资源。
 */
int lcd_init(void)
{
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;

    fb_fd = open(FB_DEV, O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", FB_DEV, strerror(errno));
        return -1;
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0) {
        perror("get framebuffer information");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    if (variable.bits_per_pixel != 16) {
        fprintf(stderr, "Framebuffer is %u bpp, RGB565 requires 16 bpp\n",
                variable.bits_per_pixel);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    lcd_width = (int)variable.xres;
    lcd_height = (int)variable.yres;
    lcd_stride = (int)fixed.line_length / 2;
    screen_size = (size_t)fixed.line_length * variable.yres;
    screen_base = mmap(NULL, screen_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fb_fd, 0);
    if (screen_base == MAP_FAILED) {
        perror("mmap framebuffer");
        screen_base = NULL;
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    staging_buffer = calloc((size_t)lcd_stride * lcd_height, sizeof(uint16_t));
    if (staging_buffer == NULL) {
        perror("allocate LCD staging buffer");
        munmap(screen_base, screen_size);
        screen_base = NULL;
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    memset(screen_base, 0, screen_size);
    printf("LCD: %d x %d, stride=%d pixels\n",
           lcd_width, lcd_height, lcd_stride);
    return 0;
}

/*
 * 将 RGB565 图像等比例缩放后逐行写入 framebuffer。
 * source_stride 以像素为单位；显示尚未初始化或参数非法时函数直接返回。
 */
void lcd_show_frame(const uint16_t *rgb565,
                    int source_width, int source_height, int source_stride)
{
    DisplayTransform transform;
    int y;

    if (screen_base == NULL || staging_buffer == NULL || rgb565 == NULL)
        return;
    if (display_transform_init(&transform, source_width, source_height,
                               lcd_width, lcd_height) < 0)
        return;

    display_render_letterbox_rgb565(staging_buffer,
                                    lcd_width, lcd_height, lcd_stride,
                                    rgb565, source_width, source_height,
                                    source_stride, &transform);
    for (y = 0; y < lcd_height; ++y) {
        memcpy(screen_base + y * lcd_stride,
               staging_buffer + y * lcd_stride,
               (size_t)lcd_width * sizeof(uint16_t));
    }
}

/* 通过非空输出参数返回 framebuffer 的可见宽度和高度。 */
void lcd_get_size(int *width, int *height)
{
    if (width != NULL)
        *width = lcd_width;
    if (height != NULL)
        *height = lcd_height;
}

/* 释放中间缓冲区、解除 framebuffer 映射并关闭设备描述符。 */
void lcd_close(void)
{
    free(staging_buffer);
    staging_buffer = NULL;
    if (screen_base != NULL) {
        munmap(screen_base, screen_size);
        screen_base = NULL;
    }
    screen_size = 0;
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}
