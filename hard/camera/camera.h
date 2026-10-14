#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>

/* 模块约定：单摄像头、单调用线程，内部保存设备及当前出队帧。
 * 顺序：init -> set_format -> init_buffers -> start_stream ->
 *       循环 get_frame/put_frame -> stop_stream -> close。
 * 接口通常以 0/-1 表示成功/失败；详细 ioctl 错误打印到 stderr。
 * 当前实现为 USERPTR，不是 MMAP；仅支持紧密排列的 RGB565/YUYV。
 */

/* 输入 device：非空设备路径，如 /dev/video0；打开并检查采集能力。
 * 返回 0 成功、-1 失败；成功后由 camera_close 关闭设备。 */
int camera_init(const char *device);

/* 无参数、无返回值；init 成功后调用，向 stdout 打印至多 32 个格式。 */
void camera_enum_formats(void);

/* 输入 width/height：期望像素尺寸；尝试 RGB565，失败后尝试 YUYV。
 * 驱动可以调整尺寸，须用 get_framesize 取实际值。尝试设置 30fps，
 * 帧率设置失败不使本函数失败。返回 0/-1；在申请缓冲区之前调用。 */
int camera_set_format(int width, int height);

/* 无输入；按实际尺寸 malloc USERPTR 缓冲区并交给驱动，不执行 mmap。
 * 返回 0/-1；部分失败后也须 camera_close 回收。 */
int camera_init_buffers(void);

/* 无输入；缓冲区就绪后 STREAMON；返回 0/-1。 */
int camera_start_stream(void);

/* 阻塞出队；输出 *data：模块持有的 RGB565 数据，*size：分配容量（字节），
 * 不是驱动 bytesused。输出指针可为 NULL。返回 0/-1。
 * 不可 free(*data)；put_frame 前用完或复制，不能跨下一次采集长期持有。 */
int camera_get_frame(void **data, size_t *size);

/* 无输入；归还上次成功 get_frame 的帧。返回 0/-1；两者须一一配对。 */
int camera_put_frame(void);

/* 无输入；STREAMOFF，返回 0/-1；释放在用缓冲区前先停流。 */
int camera_stop_stream(void);

/* 无输入/返回值；释放 USERPTR 内存、关闭 fd。调用方先停止采集和视频流。 */
void camera_close(void);

/* set_format 成功后调用；通过非空 width/height 输出实际像素尺寸，无返回值。 */
void camera_get_framesize(int *width, int *height);

#endif
