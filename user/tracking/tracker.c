/* 公开接口见 tracker.h。内部函数均由处理线程调用，不拥有输入图像。
 * clamp_int 输入值及闭区间返回钳位值；rgb565_to_gray 输入像素返回8位亮度。
 * validate_frame 输入对象/帧/尺寸/步长返回布尔值，无副作用。
 * make_thumbnail 输入原图、输出对象内缩略图；copy_patch 输出400字节模板。
 * patch_score 输入模板及缩略图位置返回SAD相似度；find_best 返回最佳 Match。
 * set_result 输入缩略图坐标/状态/得分，输出原图框与整数中心。
 * update_adaptive_template/update_tracking/update_lost_search 原地更新对象，无返回值。
 * 内部搜索要求尺寸/位置已合法。低分前两帧仍保留旧中心及TRACKING状态，
 * 第三帧才进入LOST_SEARCH；score 是灰度相似度，不是类别识别概率。
 */
#include "tracker.h"

#include <stddef.h>
#include <string.h>

#include "display_transform.h"

#define TRACKER_LOST_FRAMES 3
#define TRACKER_REACQUIRE_HITS 2
#define TRACKER_TRACK_THRESHOLD 0.70f
#define TRACKER_REACQUIRE_THRESHOLD 0.80f

typedef struct {
    int x;
    int y;
    float score;
} Match;

/* 将整数限制在闭区间 [minimum, maximum] 内。 */
static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

/* 把一个 RGB565 像素按整数亮度权重转换为 8 位灰度值。 */
static uint8_t rgb565_to_gray(uint16_t pixel)
{
    unsigned r5 = (pixel >> 11) & 0x1fU;
    unsigned g6 = (pixel >> 5) & 0x3fU;
    unsigned b5 = pixel & 0x1fU;
    unsigned r = (r5 << 3) | (r5 >> 2);
    unsigned g = (g6 << 2) | (g6 >> 4);
    unsigned b = (b5 << 3) | (b5 >> 2);
    return (uint8_t)((77U * r + 150U * g + 29U * b) >> 8);
}

/* 检查帧指针、尺寸和步长是否与跟踪器初始化参数一致。 */
static int validate_frame(const Tracker *tracker, const uint16_t *rgb565,
                          int width, int height, int stride)
{
    return tracker != NULL && rgb565 != NULL &&
           width == tracker->source_width && height == tracker->source_height &&
           stride >= width;
}

/*
 * 对 RGB565 原图进行 4×4 块平均下采样，生成供 SAD 匹配使用的灰度缩略图。
 */
static void make_thumbnail(Tracker *tracker, const uint16_t *rgb565, int stride)
{
    int thumbnail_y;

    for (thumbnail_y = 0; thumbnail_y < tracker->thumb_height; ++thumbnail_y) {
        int thumbnail_x;
        for (thumbnail_x = 0; thumbnail_x < tracker->thumb_width; ++thumbnail_x) {
            unsigned sum = 0;
            int dy;
            for (dy = 0; dy < TRACKER_SCALE; ++dy) {
                const uint16_t *row = rgb565 +
                    (thumbnail_y * TRACKER_SCALE + dy) * stride +
                    thumbnail_x * TRACKER_SCALE;
                int dx;
                for (dx = 0; dx < TRACKER_SCALE; ++dx)
                    sum += rgb565_to_gray(row[dx]);
            }
            tracker->thumbnail[thumbnail_y * tracker->thumb_width + thumbnail_x] =
                (uint8_t)(sum / (TRACKER_SCALE * TRACKER_SCALE));
        }
    }
}

/* 从缩略图指定位置复制一个 20×20 模板块到 destination。 */
static void copy_patch(const Tracker *tracker, int patch_x, int patch_y,
                       uint8_t *destination)
{
    int y;
    for (y = 0; y < TRACKER_TEMPLATE_HEIGHT; ++y) {
        memcpy(destination + y * TRACKER_TEMPLATE_WIDTH,
               tracker->thumbnail + (patch_y + y) * tracker->thumb_width + patch_x,
               TRACKER_TEMPLATE_WIDTH);
    }
}

/*
 * 计算候选块与目标模板的 SAD 相似度，1.0 表示完全相同，数值越低差异越大。
 */
static float patch_score(const Tracker *tracker, const uint8_t *target,
                         int patch_x, int patch_y)
{
    unsigned long sad = 0;
    int y;

    for (y = 0; y < TRACKER_TEMPLATE_HEIGHT; ++y) {
        const uint8_t *candidate = tracker->thumbnail +
            (patch_y + y) * tracker->thumb_width + patch_x;
        const uint8_t *reference = target + y * TRACKER_TEMPLATE_WIDTH;
        int x;
        for (x = 0; x < TRACKER_TEMPLATE_WIDTH; ++x) {
            int difference = (int)candidate[x] - (int)reference[x];
            sad += (unsigned long)(difference < 0 ? -difference : difference);
        }
    }

    return 1.0f - (float)sad /
        (float)(TRACKER_TEMPLATE_PIXELS * 255UL);
}

/* 在限定的缩略图搜索区域内遍历候选块，并返回相似度最高的位置。 */
static Match find_best(const Tracker *tracker, const uint8_t *target,
                       int minimum_x, int minimum_y,
                       int maximum_x, int maximum_y)
{
    Match best = {0, 0, -1.0f};
    int max_patch_x = tracker->thumb_width - TRACKER_TEMPLATE_WIDTH;
    int max_patch_y = tracker->thumb_height - TRACKER_TEMPLATE_HEIGHT;
    int y;

    minimum_x = clamp_int(minimum_x, 0, max_patch_x);
    minimum_y = clamp_int(minimum_y, 0, max_patch_y);
    maximum_x = clamp_int(maximum_x, 0, max_patch_x);
    maximum_y = clamp_int(maximum_y, 0, max_patch_y);

    for (y = minimum_y; y <= maximum_y; ++y) {
        int x;
        for (x = minimum_x; x <= maximum_x; ++x) {
            float score = patch_score(tracker, target, x, y);
            if (score > best.score) {
                best.x = x;
                best.y = y;
                best.score = score;
            }
        }
    }
    return best;
}

/* 将缩略图坐标换算为原图 80×80 跟踪框，并更新状态、中心和得分。 */
static void set_result(TrackerResult *result, TrackerState state,
                       int thumbnail_x, int thumbnail_y, float score)
{
    result->state = state;
    result->x1 = thumbnail_x * TRACKER_SCALE;
    result->y1 = thumbnail_y * TRACKER_SCALE;
    result->x2 = result->x1 + TRACKER_BOX_SIZE - 1;
    result->y2 = result->y1 + TRACKER_BOX_SIZE - 1;
    result->center_x = (result->x1 + result->x2) / 2;
    result->center_y = (result->y1 + result->y2) / 2;
    result->score = score;
}

/*
 * 用高置信度候选块缓慢更新自适应模板，降低外观渐变造成的跟踪漂移。
 */
static void update_adaptive_template(Tracker *tracker, int x, int y)
{
    uint8_t patch[TRACKER_TEMPLATE_PIXELS];
    int i;
    copy_patch(tracker, x, y, patch);
    for (i = 0; i < TRACKER_TEMPLATE_PIXELS; ++i) {
        tracker->adaptive_template[i] =
            (uint8_t)((15U * tracker->adaptive_template[i] + patch[i]) / 16U);
    }
}

/*
 * 初始化跟踪器尺寸和空闲状态。
 * 图像过小、超过静态缩略图容量或参数非法时返回 -1，否则返回 0。
 */
int tracker_init(Tracker *tracker, int source_width, int source_height)
{
    if (tracker == NULL || source_width <= 0 || source_height <= 0 ||
        source_width / TRACKER_SCALE > TRACKER_THUMB_MAX_WIDTH ||
        source_height / TRACKER_SCALE > TRACKER_THUMB_MAX_HEIGHT ||
        source_width < TRACKER_BOX_SIZE || source_height < TRACKER_BOX_SIZE)
        return -1;

    memset(tracker, 0, sizeof(*tracker));
    tracker->source_width = source_width;
    tracker->source_height = source_height;
    tracker->thumb_width = source_width / TRACKER_SCALE;
    tracker->thumb_height = source_height / TRACKER_SCALE;
    tracker->result.state = TRACKER_IDLE;
    tracker->last_valid.state = TRACKER_IDLE;
    return 0;
}

/*
 * 以用户选点为中心截取初始目标模板并进入 TRACKER_TRACKING 状态。
 * 输入帧必须匹配初始化尺寸；成功返回 0，参数非法时返回 -1。
 */
int tracker_select(Tracker *tracker, const uint16_t *rgb565,
                   int width, int height, int stride,
                   int center_x, int center_y)
{
    int x1;
    int y1;
    int x2;
    int y2;
    int thumbnail_x;
    int thumbnail_y;

    if (!validate_frame(tracker, rgb565, width, height, stride))
        return -1;

    make_thumbnail(tracker, rgb565, stride);
    display_box_from_center(width, height, TRACKER_BOX_SIZE,
                            center_x, center_y, &x1, &y1, &x2, &y2);
    (void)x2;
    (void)y2;
    thumbnail_x = clamp_int(x1 / TRACKER_SCALE, 0,
                            tracker->thumb_width - TRACKER_TEMPLATE_WIDTH);
    thumbnail_y = clamp_int(y1 / TRACKER_SCALE, 0,
                            tracker->thumb_height - TRACKER_TEMPLATE_HEIGHT);
    copy_patch(tracker, thumbnail_x, thumbnail_y, tracker->reference_template);
    memcpy(tracker->adaptive_template, tracker->reference_template,
           sizeof(tracker->adaptive_template));
    set_result(&tracker->result, TRACKER_TRACKING,
               thumbnail_x, thumbnail_y, 1.0f);
    tracker->last_valid = tracker->result;
    tracker->low_score_frames = 0;
    tracker->lost_search_frames = 0;
    tracker->reacquire_hits = 0;
    return 0;
}

/*
 * 在上一有效位置附近进行局部 SAD 搜索。
 * 连续低于阈值三帧后转入丢失搜索，高置信度时更新自适应模板。
 */
static void update_tracking(Tracker *tracker)
{
    int previous_x = tracker->last_valid.x1 / TRACKER_SCALE;
    int previous_y = tracker->last_valid.y1 / TRACKER_SCALE;
    Match best = find_best(tracker, tracker->adaptive_template,
                           previous_x - 25, previous_y - 25,
                           previous_x + 25, previous_y + 25);

    if (best.score >= TRACKER_TRACK_THRESHOLD) {
        set_result(&tracker->result, TRACKER_TRACKING,
                   best.x, best.y, best.score);
        tracker->last_valid = tracker->result;
        tracker->low_score_frames = 0;
        if (best.score >= 0.85f)
            update_adaptive_template(tracker, best.x, best.y);
        return;
    }

    tracker->low_score_frames++;
    tracker->result = tracker->last_valid;
    tracker->result.score = best.score;
    if (tracker->low_score_frames >= TRACKER_LOST_FRAMES) {
        tracker->result.state = TRACKER_LOST_SEARCH;
        tracker->lost_search_frames = 0;
        tracker->reacquire_hits = 0;
    }
}

/*
 * 按逐步扩大范围的策略搜索原始参考模板，并要求连续命中后才重新捕获。
 */
static void update_lost_search(Tracker *tracker)
{
    int previous_x = tracker->last_valid.x1 / TRACKER_SCALE;
    int previous_y = tracker->last_valid.y1 / TRACKER_SCALE;
    Match best;
    int performed_search = 1;

    tracker->lost_search_frames++;
    if (tracker->lost_search_frames == 1) {
        best = find_best(tracker, tracker->reference_template,
                         previous_x - 25, previous_y - 25,
                         previous_x + 25, previous_y + 25);
    } else if (tracker->lost_search_frames == 2) {
        best = find_best(tracker, tracker->reference_template,
                         previous_x - 40, previous_y - 40,
                         previous_x + 40, previous_y + 40);
    } else if (tracker->lost_search_frames % 3 == 0) {
        best = find_best(tracker, tracker->reference_template,
                         0, 0, tracker->thumb_width, tracker->thumb_height);
    } else {
        best.x = 0;
        best.y = 0;
        best.score = tracker->result.score;
        performed_search = 0;
    }

    tracker->result = tracker->last_valid;
    tracker->result.state = TRACKER_LOST_SEARCH;
    tracker->result.score = best.score;
    if (!performed_search)
        return;

    if (best.score >= TRACKER_REACQUIRE_THRESHOLD) {
        if (tracker->reacquire_hits > 0 &&
            (best.x < tracker->pending_reacquire_x - 2 ||
             best.x > tracker->pending_reacquire_x + 2 ||
             best.y < tracker->pending_reacquire_y - 2 ||
             best.y > tracker->pending_reacquire_y + 2)) {
            tracker->reacquire_hits = 0;
        }
        tracker->pending_reacquire_x = best.x;
        tracker->pending_reacquire_y = best.y;
        tracker->reacquire_hits++;
        if (tracker->reacquire_hits >= TRACKER_REACQUIRE_HITS) {
            set_result(&tracker->result, TRACKER_TRACKING,
                       best.x, best.y, best.score);
            tracker->last_valid = tracker->result;
            copy_patch(tracker, best.x, best.y, tracker->adaptive_template);
            tracker->low_score_frames = 0;
            tracker->lost_search_frames = 0;
            tracker->reacquire_hits = 0;
        }
    } else {
        tracker->reacquire_hits = 0;
    }
}

/*
 * 使用新帧推进跟踪状态机，并把当前结果写入 result。
 * 输入帧不匹配时返回 -1；空闲状态下返回空闲结果而不执行搜索。
 */
int tracker_update(Tracker *tracker, const uint16_t *rgb565,
                   int width, int height, int stride,
                   TrackerResult *result)
{
    if (!validate_frame(tracker, rgb565, width, height, stride) || result == NULL)
        return -1;
    if (tracker->result.state == TRACKER_IDLE) {
        *result = tracker->result;
        return 0;
    }

    make_thumbnail(tracker, rgb565, stride);
    if (tracker->result.state == TRACKER_TRACKING)
        update_tracking(tracker);
    else
        update_lost_search(tracker);
    *result = tracker->result;
    return 0;
}

/* 将跟踪器当前结果复制到非空的 result 输出参数。 */
void tracker_get_result(const Tracker *tracker, TrackerResult *result)
{
    if (tracker != NULL && result != NULL)
        *result = tracker->result;
}
