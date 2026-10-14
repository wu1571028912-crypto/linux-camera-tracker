/* 单设备 USERPTR 采集，公开函数契约见 camera.h。
 * yuyv_to_rgb565 输入可写紧密 YUYV 和像素尺寸，原地输出 RGB565，无返回值。
 * 当前限制：未适配 bytesperline/sizeimage，未检查 DQBUF 索引，驱动返回的
 * reqbuf.count 超过固定数组容量时有越界风险；本次注释不表示这些问题已修复。
 */
#include "camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define FRAMEBUFFER_COUNT 3   // 缓冲区数量

typedef struct cam_buf_info {
    unsigned char *start;     // 用户空间缓冲区地址
    size_t length;            // 缓冲区长度
} cam_buf_info;

static cam_buf_info buf_infos[FRAMEBUFFER_COUNT];
static int buf_count = 0;
static int v4l2_pixelformat = 0;   // 实际像素格式
static int frm_width, frm_height;  // 实际帧宽高

static int v4l2_fd = -1;

static struct v4l2_buffer current_buf;
static int current_buf_valid = 0;

/*
 * 打开指定 V4L2 设备并检查视频采集能力。
 * 打开设备、查询设备基本能力
 * device 为设备节点路径；成功返回 0，失败返回 -1 并关闭已打开的描述符。
 */
int camera_init(const char *device)
{
    struct v4l2_capability cap;
    v4l2_fd = open(device, O_RDWR);
    if (v4l2_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
        return -1;
    }

    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is not a video capture device\n", device);
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // 打印能力，便于调试
    if (cap.capabilities & V4L2_CAP_STREAMING)
        printf("Device supports streaming I/O\n");
    else
        printf("Device does NOT support streaming I/O, will use read()\n");

    return 0;
}


/*
 * 将一帧 YUYV 图像原地转换为 RGB565。
 * data 必须指向每像素 2 字节的可写缓冲区，转换后缓冲区大小保持不变。
 */
static void yuyv_to_rgb565(unsigned char *data, int width, int height)
{
    for (int y = 0; y < height; y++) {
        unsigned char *row = data + y * width * 2;  // 每行 width*2 字节
        for (int x = 0; x < width - 1; x += 2) {
            int y0 = row[x * 2];
            int u  = row[x * 2 + 1];
            int y1 = row[x * 2 + 2];
            int v  = row[x * 2 + 3];

            // 第一个像素
            int c = y0 - 16;
            int d = u - 128;
            int e = v - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
            r = (r < 0) ? 0 : (r > 255 ? 255 : r);
            g = (g < 0) ? 0 : (g > 255 ? 255 : g);
            b = (b < 0) ? 0 : (b > 255 ? 255 : b);
            unsigned short rgb565_0 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

            // 第二个像素
            c = y1 - 16;
            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;
            r = (r < 0) ? 0 : (r > 255 ? 255 : r);
            g = (g < 0) ? 0 : (g > 255 ? 255 : g);
            b = (b < 0) ? 0 : (b > 255 ? 255 : b);
            unsigned short rgb565_1 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

            // 写入原位置（小端）
            row[x * 2]     = rgb565_0 & 0xFF;
            row[x * 2 + 1] = (rgb565_0 >> 8) & 0xFF;
            row[x * 2 + 2] = rgb565_1 & 0xFF;
            row[x * 2 + 3] = (rgb565_1 >> 8) & 0xFF;
        }
        // 如果宽度为奇数，处理最后一个像素（简单复制前一个或单独处理）
        if (width % 2 == 1) {
            int x = width - 1;
            // 简单复制前一个像素的颜色
            row[x * 2]     = row[(x - 1) * 2];
            row[x * 2 + 1] = row[(x - 1) * 2 + 1];
        }
    }
}


/* 枚举并打印当前摄像头支持的采集格式，最多显示 32 项。 */
void camera_enum_formats(void)
{
    struct v4l2_fmtdesc fmtdesc;
    int index = 0;

    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    printf("Supported formats:\n");
    while (index < 32) {   // 限制最多枚举32种
        fmtdesc.index = index;
        if (ioctl(v4l2_fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0)
            break;
        printf("  0x%08x : %s\n", fmtdesc.pixelformat, fmtdesc.description);
        index++;
        memset(&fmtdesc, 0, sizeof(fmtdesc));
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }
}

/*
 * 请求采集尺寸、RGB565/YUYV 格式和 30 fps 帧率。
 * 设置尺寸、像素格式和帧率
 * width、height 是期望值；驱动协商后的实际尺寸会保存到模块状态中。
 */
int camera_set_format(int width, int height)
{
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;  // 先尝试 RGB565
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT RGB565");
        // 尝试 YUYV
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT YUYV");
            return -1;
        }
    }

    // 检查最终格式
    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565) {
        v4l2_pixelformat = V4L2_PIX_FMT_RGB565;
        printf("Using RGB565 format\n");
    } else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
        v4l2_pixelformat = V4L2_PIX_FMT_YUYV;
        printf("Using YUYV format, will convert to RGB565\n");
    } else {
        fprintf(stderr, "Unsupported pixel format: 0x%08x\n", fmt.fmt.pix.pixelformat);
        return -1;
    }

    frm_width = fmt.fmt.pix.width;
    frm_height = fmt.fmt.pix.height;
    printf("Actual frame size: %d x %d\n", frm_width, frm_height);

    /* 设置帧率 30fps */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    //视频采集
    if (ioctl(v4l2_fd, VIDIOC_G_PARM, &parm) == 0) {

        if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = 30;
            if (ioctl(v4l2_fd, VIDIOC_S_PARM, &parm) < 0)
                perror("VIDIOC_S_PARM (frame rate)");
        }

    }
    return 0;
}

/*
 * 申请三个 V4L2 USERPTR 缓冲区并全部入队。
 * 申请并提交采集缓冲区
 * 成功返回 0；失败返回 -1，调用者随后应通过 camera_close 统一释放资源。
 */
int camera_init_buffers(void)
{
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_buffer buf;

    // 使用 userptr 方式
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = FRAMEBUFFER_COUNT;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        perror("VIDIOC_REQBUFS (userptr)");
        return -1;
    }

    if (reqbuf.count < FRAMEBUFFER_COUNT) {
        fprintf(stderr, "Not enough buffers: %u\n", reqbuf.count);
        return -1;
    }
    buf_count = reqbuf.count;

    // 为每个缓冲区分配用户空间内存
    size_t buf_size = frm_width * frm_height * 2; // YUYV 每像素 2 字节
    for (int i = 0; i < buf_count; i++) {
        buf_infos[i].length = buf_size;
        buf_infos[i].start = malloc(buf_size);
        if (!buf_infos[i].start) {
            perror("malloc");
            return -1;
        }

        // 初始化 v4l2_buffer 并设置 userptr
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;
        buf.m.userptr = (unsigned long)buf_infos[i].start;
        buf.length = buf_infos[i].length;

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF (userptr)");
            return -1;
        }
    }

    printf("Userptr mode initialized, %d buffers, size %zu each\n",
           buf_count, buf_size);
    return 0;
}


/* 启动 V4L2 视频流；成功返回 0，VIDIOC_STREAMON 失败时返回 -1。 */
int camera_start_stream(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}

/* 停止 V4L2 视频流；成功返回 0，VIDIOC_STREAMOFF 失败时返回 -1。 */
int camera_stop_stream(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
        return -1;
    }
    return 0;
}


/*
 * 从驱动出队一帧，并通过 data、size 返回模块所有的缓冲区及其长度。
 * 若采集格式为 YUYV，会先原地转换为 RGB565；使用完必须调用 camera_put_frame。
 */
int camera_get_frame(void **data, size_t *size)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;   // 注意使用 userptr

    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return -1;
    }

    // 如果是 YUYV 格式，原地转换为 RGB565
    if (v4l2_pixelformat == V4L2_PIX_FMT_YUYV) {
        yuyv_to_rgb565(buf_infos[buf.index].start, frm_width, frm_height);
    }

    if (data) *data = buf_infos[buf.index].start;
    if (size) *size = buf_infos[buf.index].length;

    current_buf = buf;        // 保存当前 buffer
    current_buf_valid = 1;
    return 0;
}

/*
 * 将最近一次 camera_get_frame 取出的缓冲区重新入队。
 * 当前没有待归还帧或 VIDIOC_QBUF 失败时返回 -1。
 */
int camera_put_frame(void)
{
    if (!current_buf_valid) return -1;
    if (ioctl(v4l2_fd, VIDIOC_QBUF, &current_buf) < 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    current_buf_valid = 0;
    return 0;
}

/* 释放所有 USERPTR 缓冲区并关闭摄像头描述符，可在部分初始化后调用。 */
void camera_close(void)
{
    // 释放 userptr 缓冲区
    for (int i = 0; i < buf_count; i++) {
        free(buf_infos[i].start);
        buf_infos[i].start = NULL;
    }
    buf_count = 0;
    if (v4l2_fd >= 0) {
        close(v4l2_fd);
        v4l2_fd = -1;
    }
    current_buf_valid = 0;
}

/* 通过非空输出参数返回驱动协商后的实际帧宽和帧高。 */
void camera_get_framesize(int *width, int *height)
{
    if (width)  *width  = frm_width;
    if (height) *height = frm_height;
}
