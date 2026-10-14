#ifndef TRACKING_LOG_H
#define TRACKING_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "tracker.h"

/* 输入 current/previous：本帧/上帧结果；now_ms：单调毫秒。
 * 输入输出 last_emit_ms：上次生成日志的时间，首次设 0。
 * 输出 destination：调用方缓冲区，destination_size 为字节容量。
 * 返回 1 已生成 NUL 结尾文本、0 因 IDLE/100ms 节流跳过、-1 参数/容量错误。
 * 只格式化，不发送串口、不打印、不加换行；返回非 1 时不可当成新日志使用。 */
int tracking_format_line(char *destination, size_t destination_size,
                         const TrackerResult *current,
                         const TrackerResult *previous,
                         uint64_t now_ms, uint64_t *last_emit_ms);

#endif
