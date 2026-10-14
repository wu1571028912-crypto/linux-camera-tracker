/* 文本格式化模块，公开契约见 tracking_log.h，不执行串口 IO。
 * write_line 输入容量/时间/前缀/结果，输出 destination 和 last_emit_ms，
 * 返回1成功、-1格式化或容量失败，不附加换行。
 */
#include "tracking_log.h"

#include <stdio.h>

#define TRACKING_LOG_INTERVAL_MS 100U

/*
 * 按指定前缀格式化一行跟踪日志，并在成功时更新时间戳。
 * 缓冲区不足或格式化失败时返回 -1，成功返回 1。
 */
static int write_line(char *destination, size_t destination_size,
                      uint64_t now_ms, uint64_t *last_emit_ms,
                      const char *prefix, const TrackerResult *result)
{
    int length;

    if (prefix[0] == 'L') {
        length = snprintf(destination, destination_size,
                          "LOST_SEARCH last_x1=%d last_y1=%d last_x2=%d last_y2=%d last_cx=%d last_cy=%d",
                          result->x1, result->y1, result->x2, result->y2,
                          result->center_x, result->center_y);
    } else {
        length = snprintf(destination, destination_size,
                          "%s x1=%d y1=%d x2=%d y2=%d cx=%d cy=%d score=%.2f",
                          prefix, result->x1, result->y1, result->x2, result->y2,
                          result->center_x, result->center_y,
                          (double)result->score);
    }

    if (length < 0 || (size_t)length >= destination_size)
        return -1;

    *last_emit_ms = now_ms;
    return 1;
}

/*
 * 根据状态变化和 100 ms 节流规则决定是否生成日志。
 * 返回 1 表示已写入，0 表示本次无需输出，-1 表示参数或格式化错误。
 */
int tracking_format_line(char *destination, size_t destination_size,
                         const TrackerResult *current,
                         const TrackerResult *previous,
                         uint64_t now_ms, uint64_t *last_emit_ms)
{
    int state_changed;

    if (!destination || destination_size == 0 || !current || !previous ||
        !last_emit_ms)
        return -1;

    if (current->state == TRACKER_IDLE)
        return 0;

    state_changed = current->state != previous->state;
    if (!state_changed && now_ms >= *last_emit_ms &&
        now_ms - *last_emit_ms < TRACKING_LOG_INTERVAL_MS)
        return 0;

    if (current->state == TRACKER_LOST_SEARCH)
        return write_line(destination, destination_size, now_ms, last_emit_ms,
                          "LOST_SEARCH", current);

    return write_line(destination, destination_size, now_ms, last_emit_ms,
                      previous->state == TRACKER_LOST_SEARCH ?
                          "REACQUIRED" : "TRACKING",
                      current);
}
