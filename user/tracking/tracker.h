#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>

/* 单目标灰度模板匹配，不是目标类别识别。init -> select -> update/get_result。
 * 同一个 Tracker 只由一个线程使用；所有坐标均为原图像素，左上为原点。
 * 输入 RGB565 只读，stride 单位为像素；模板保存在对象内，不持有输入图像。 */

#define TRACKER_BOX_SIZE 80
#define TRACKER_SCALE 4
#define TRACKER_THUMB_MAX_WIDTH 160
#define TRACKER_THUMB_MAX_HEIGHT 120
#define TRACKER_TEMPLATE_WIDTH 20
#define TRACKER_TEMPLATE_HEIGHT 20
#define TRACKER_TEMPLATE_PIXELS (TRACKER_TEMPLATE_WIDTH * TRACKER_TEMPLATE_HEIGHT)

typedef enum {
    TRACKER_IDLE = 0,
    TRACKER_TRACKING,
    TRACKER_LOST_SEARCH
} TrackerState;

typedef struct {
    TrackerState state;
    int x1;
    int y1;
    int x2;
    int y2;
    int center_x;
    int center_y;
    float score;
} TrackerResult;

typedef struct {
    /*
     * 相机原始图像的宽度和高度，单位：原图像素。
     *
     * 例如相机输出640×480：
     * source_width  = 640
     * source_height = 480
     *
     * tracker_select()和tracker_update()会检查传入图像的
     * 宽高是否与这里保存的宽高一致。
     */
    int source_width;
    int source_height;

    /*
     * 下采样灰度图的宽度和高度，单位：缩略图像素。
     *
     * 计算方式：
     * thumb_width  = source_width  / TRACKER_SCALE
     * thumb_height = source_height / TRACKER_SCALE
     *
     * 当前TRACKER_SCALE为4，因此640×480原图会生成：
     * 160×120缩略图。
     */
    int thumb_width;
    int thumb_height;

    /*
     * 当前帧经过灰度化和下采样后得到的缩略图。
     *
     * 每个元素是一个0～255的灰度值，按行连续保存：
     *
     * thumbnail[y * thumb_width + x]
     *
     * 当前最大空间为：
     * 160 × 120 = 19200字节
     *
     * 每次调用tracker_select()或者tracker_update()时，
     * make_thumbnail()都会使用当前相机画面更新这里的数据。
     */
    uint8_t thumbnail[
        TRACKER_THUMB_MAX_WIDTH * TRACKER_THUMB_MAX_HEIGHT
    ];

    /*
     * 用户第一次点击选择目标时截取的原始模板。
     *
     * 当前模板大小为20×20灰度像素：
     * TRACKER_TEMPLATE_PIXELS = 20 × 20 = 400
     *
     * 它对应原图上的80×80区域，因为：
     * 20 × TRACKER_SCALE(4) = 80
     *
     * 该模板在正常跟踪过程中不会更新，主要用于目标丢失后
     * 重新搜索，避免自适应模板逐渐漂移后无法找回原目标。
     */
    uint8_t reference_template[TRACKER_TEMPLATE_PIXELS];

    /*
     * 正常跟踪时使用的自适应模板。
     *
     * 初次选择目标时，它和reference_template完全相同。
     *
     * 当匹配分数足够高时，程序会使用当前目标图像缓慢更新它：
     *
     * new_template =
     *     (15 * old_template + current_patch) / 16
     *
     * 这样可以适应目标的轻微旋转、光照变化以及相机曝光变化，
     * 但也存在长时间跟踪后模板漂移的可能。
     */
    uint8_t adaptive_template[TRACKER_TEMPLATE_PIXELS];

    /*
     * 当前一帧的跟踪结果。
     *
     * 其中保存：
     * - 当前跟踪状态
     * - 方框左上角和右下角坐标
     * - 方框中心坐标
     * - 当前最佳匹配分数
     *
     * TrackerResult中的方框坐标都是原始相机图像坐标，
     * 不是缩略图坐标。
     *
     * 当目标丢失时，方框位置仍保持在最后一次可靠位置，
     * 但state会变为TRACKER_LOST_SEARCH。
     */
    TrackerResult result;

    /*
     * 最后一次可靠匹配到目标时的跟踪结果。
     *
     * 只有匹配分数达到正常跟踪门槛后才会更新。
     * 当当前帧匹配失败时，程序用它保留目标最后出现的位置。
     *
     * 因此目标丢失后，仍然能够输出最后一次有效的：
     * - 左上角坐标
     * - 右下角坐标
     * - 中心点坐标
     *
     * 这里的坐标同样是原始相机图像坐标。
     */
    TrackerResult last_valid;

    /*
     * 连续低匹配分数的帧数。
     *
     * 正常跟踪时，如果最佳匹配分数低于
     * TRACKER_TRACK_THRESHOLD（当前为0.70），该值加1。
     *
     * 匹配成功后清零。
     *
     * 当连续低分帧达到TRACKER_LOST_FRAMES（当前为3）时，
     * 跟踪器从TRACKER_TRACKING切换到TRACKER_LOST_SEARCH。
     */
    int low_score_frames;

    /*
     * 进入目标丢失状态后已经经过的搜索帧数。
     *
     * 它用于控制搜索范围：
     *
     * 第1次：在最后位置附近±25个缩略图像素搜索
     * 第2次：扩大到±40个缩略图像素搜索
     * 以后：每隔3帧执行一次全图搜索
     *
     * 重新找到目标后清零。
     */
    int lost_search_frames;

    /*
     * 重新捕获目标时连续稳定匹配的次数。
     *
     * 丢失搜索中，即使某一帧找到高分区域，也不会立即认定
     * 已经找回目标，需要连续多次在相近位置匹配成功。
     *
     * 当前TRACKER_REACQUIRE_HITS为2，即至少需要两次稳定匹配。
     *
     * 如果匹配分数不足或者两次位置差距太大，该值清零。
     */
    int reacquire_hits;

    /*
     * 上一次重新捕获候选区域的左上角X坐标。
     *
     * 注意：这里保存的是缩略图坐标，不是原图坐标。
     *
     * 用于比较下一次高分匹配是否仍然出现在相近位置。
     */
    int pending_reacquire_x;

    /*
     * 上一次重新捕获候选区域的左上角Y坐标。
     *
     * 注意：这里也是缩略图坐标。
     *
     * 如果新的候选位置与该位置相差不超过±2个缩略图像素，
     * 才认为两次匹配位置基本一致。
     *
     * ±2个缩略图像素约等于原图中的±8像素。
     */
    int pending_reacquire_y;

} Tracker;

/* 输入 tracker：输出对象；source_width/height：源图尺寸，至少 80x80，
 * 下采样尺寸不得超过静态容量。返回 0 成功、-1 非法参数；成功后为 IDLE。 */
int tracker_init(Tracker *tracker, int source_width, int source_height);

/* 输入已初始化 tracker、匹配其尺寸的 RGB565 图及像素步长、原图中心点。
 * 从中心附近截取 80x80（边界处平移）模板；输出写回 tracker，进入 TRACKING。
 * 返回 0 成功、-1 参数错误；无需保留输入帧。 */
int tracker_select(Tracker *tracker, const uint16_t *rgb565,
                   int width, int height, int stride,
                   int center_x, int center_y);
/* 输入 tracker 和新帧，输出 *result（框、中心、状态、SAD 相似度）。
 * 返回 0 表示计算成功，不代表找到目标；必须检查 result->state。
 * 返回 -1 表示参数错误；IDLE 时不会自动选目标。 */
int tracker_update(Tracker *tracker, const uint16_t *rgb565,
                   int width, int height, int stride,
                   TrackerResult *result);
/* 输入 tracker，输出 *result；无返回值，不推进算法；任一指针为 NULL 时不写。 */
void tracker_get_result(const Tracker *tracker, TrackerResult *result);

#endif
