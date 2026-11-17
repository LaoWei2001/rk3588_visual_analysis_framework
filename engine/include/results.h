/** @file results.h
 * @brief 不依赖视频通道的目标/ROI 查询；借用结果，不复制图像或目标。
 */
#pragma once
#include <config_types.h>
#include <types.h>

/* ROI_ALL 保持既有语义：区域并集，没有区域时为整帧。
 * ROI_FRAME 明确忽略区域；ROI_NONE 表示名称查询失败，绝不能当成整帧。 */
enum
{
    ROI_ALL = -1,
    ROI_NONE = -2,
    ROI_FRAME = -3
};

enum class TargetAnchor
{
    Center,      /* 框中心，整数宽/高的一半，与既有 ROI 查询一致。 */
    BottomCenter /* 框底边中点，常用于判断脚点是否进入区域。 */
};

/** 所有条件同时满足；labels 中任意一个标签命中即可，空列表表示不限类别。
 * 默认查整帧，min_score 为有限的 [0,1] 数值，阈值包含等号。
 * roi 可以是 ROI_FRAME、ROI_ALL，或 roi_find() 返回的区域编号。
 * 标签列表由调用方持有，可保存在业务 state 中复用。 */
struct TargetQuery
{
    std::vector<std::string> labels;
    float min_score = 0.0f;
    int roi = ROI_FRAME;
    TargetAnchor anchor = TargetAnchor::Center;
    std::string model_id;   // optional: configured model node ID
    std::string model_type; // optional: yolov8_obb / yolov5_seg / ...
};

enum class TargetQueryStatus
{
    OK,
    INFERENCE_UNAVAILABLE,
    ROI_NOT_FOUND,
    INVALID_ROI,  /* 指定区域不足三个顶点；ROI_ALL 要求所有已配置区域均满足此条件。 */
    INVALID_QUERY /* 非法置信度、区域选择值或判断点枚举。 */
};

struct TargetQueryResult
{
    TargetQueryStatus status = TargetQueryStatus::INFERENCE_UNAVAILABLE;
    int count = 0;
    /* 当前回调内借用的最高置信度目标；同分取原结果顺序中的第一个。
     * 不复制目标/图像，不得缓存指针跨帧使用或在使用前修改 results 容器。 */
    const AlgoResult *best = nullptr;

    /** @brief 查询状态为 OK 时返回 true；匹配数为 0 也可能是有效结果。
     * 示例：
     * @code
     * TargetQueryResult r = ctx->query_targets();
     * if (r.valid())
     * {
     *     int count = r.count;
     *     (void)count;
     * }
     * @endcode
     */
    bool valid() const
    {
        return status == TargetQueryStatus::OK;
    }
};

/** @brief 返回稳定英文状态名；未知枚举为 "unknown"，返回字符串无需释放。
 * 示例：
 * @code
 * const char *status = target_query_status_name(ctx->query_targets().status);
 * @endcode
 */
const char *target_query_status_name(TargetQueryStatus status);

namespace vision
{
/** @brief 将归一化 ROI 换算到 size 像素坐标；去重闭合端点，跳过不足三点的区域。
 * @return 自有数据；非正 size 返回空列表。输入坐标应已校验为有限的 [0,1]。
 * 示例：
 * @code
 * RoiZoneConfig z;
 * z.polygon = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
 * auto rois = vision::make_roi_zones({z});
 * @endcode
 */
std::vector<RoiZone> make_roi_zones(const std::vector<RoiZoneConfig> &zones,
                                    cv::Size size = business_coordinates::size());

/** @brief 直接查询检测容器，与 ChannelContext 使用同一实现；空 rois 表示未配置区域。
 * @return 空 results/失效推理返回不可用状态；best 借用输入元素，修改/销毁容器后失效。
 * 示例：
 * @code
 * std::vector<AlgoResult> detections;
 * auto r = vision::query_targets(&detections); // 有效空结果
 * @endcode
 */
TargetQueryResult query_targets(const std::vector<AlgoResult> *results, const std::vector<RoiZone> *rois = nullptr,
                                const TargetQuery &query = TargetQuery(), bool inference_valid = true);
} // namespace vision
