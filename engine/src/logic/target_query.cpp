/** @file target_query.cpp 目标筛选、ROI 构造，以及通道上下文的区域查询。 */
#include <algorithm>
#include <channel.h>
#include <cstring>
#include <opencv2/imgproc.hpp>
#include <results.h>
#include <utility>

namespace
{
bool box_in_polygon(const cv::Rect &box, const std::vector<cv::Point> &polygon, TargetAnchor anchor)
{
    if (polygon.size() < 3)
        return false;
    // Convert before addition so malformed coordinates cannot overflow signed integers.
    const double x = static_cast<double>(box.x) + box.width / 2;
    const double y = static_cast<double>(box.y) + (anchor == TargetAnchor::Center ? box.height / 2 : box.height);
    return cv::pointPolygonTest(polygon, cv::Point2f(x, y), false) >= 0;
}

// 本项目启用 -ffast-math，std::isfinite 可能被优化掉；按 IEEE-754 位模式检查。
bool finite_confidence(float value)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "confidence requires 32-bit float");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000U) != 0x7f800000U;
}

} // namespace

std::vector<RoiZone> vision::make_roi_zones(const std::vector<RoiZoneConfig> &zones, cv::Size size)
{
    std::vector<RoiZone> output;
    if (size.width <= 0 || size.height <= 0)
        return output;
    output.reserve(zones.size());
    for (const auto &configured : zones)
    {
        const auto &polygon = configured.polygon;
        size_t point_count = polygon.size();
        while (point_count > 1 && polygon.front() == polygon[point_count - 1])
            --point_count;
        if (point_count < 3)
            continue;
        RoiZone zone;
        zone.name = configured.name;
        zone.polygon.reserve(point_count);
        for (size_t i = 0; i < point_count; ++i)
        {
            const auto &point = polygon[i];
            zone.polygon.emplace_back(static_cast<int>(point.first * size.width + 0.5),
                                      static_cast<int>(point.second * size.height + 0.5));
        }
        output.push_back(std::move(zone));
    }
    return output;
}

const char *target_query_status_name(TargetQueryStatus status)
{
    switch (status)
    {
    case TargetQueryStatus::OK:
        return "ok";
    case TargetQueryStatus::INFERENCE_UNAVAILABLE:
        return "inference_unavailable";
    case TargetQueryStatus::ROI_NOT_FOUND:
        return "roi_not_found";
    case TargetQueryStatus::INVALID_ROI:
        return "invalid_roi";
    case TargetQueryStatus::INVALID_QUERY:
        return "invalid_query";
    }
    return "unknown";
}

TargetQueryResult vision::query_targets(const std::vector<AlgoResult> *results, const std::vector<RoiZone> *rois,
                                        const TargetQuery &query, bool inference_valid)
{
    TargetQueryResult out;
    if (!finite_confidence(query.min_score) || query.min_score < 0.0f || query.min_score > 1.0f ||
        (query.anchor != TargetAnchor::Center && query.anchor != TargetAnchor::BottomCenter) ||
        (query.roi < 0 && query.roi != ROI_FRAME && query.roi != ROI_ALL && query.roi != ROI_NONE))
    {
        out.status = TargetQueryStatus::INVALID_QUERY;
        return out;
    }

    const RoiZone *zone = nullptr;
    if (query.roi == ROI_NONE || query.roi >= 0)
    {
        zone = (rois && query.roi >= 0 && query.roi < static_cast<int>(rois->size())) ? &(*rois)[query.roi] : nullptr;
        if (!zone)
        {
            out.status = TargetQueryStatus::ROI_NOT_FOUND;
            return out;
        }
        if (zone->polygon.size() < 3)
        {
            out.status = TargetQueryStatus::INVALID_ROI;
            return out;
        }
    }
    if (query.roi == ROI_ALL && rois)
        for (const auto &candidate : *rois)
            if (candidate.polygon.size() < 3)
            {
                out.status = TargetQueryStatus::INVALID_ROI;
                return out;
            }

    if (!inference_valid || !results)
        return out;

    out.status = TargetQueryStatus::OK;
    for (const auto &target : *results)
    {
        if ((!query.model_id.empty() && target.model_id != query.model_id) ||
            (!query.model_type.empty() && target.model_type != query.model_type))
            continue;
        if (target.is_depth() || !finite_confidence(target.score) || target.score < query.min_score ||
            target.score > 1.0f || target.box.width <= 0 || target.box.height <= 0)
            continue;
        if (!query.labels.empty() &&
            std::find(query.labels.begin(), query.labels.end(), target.label) == query.labels.end())
            continue;
        if (zone && !box_in_polygon(target.box, zone->polygon, query.anchor))
            continue;
        if (query.roi == ROI_ALL && rois && !rois->empty())
        {
            bool inside = false;
            for (const auto &candidate : *rois)
                if (box_in_polygon(target.box, candidate.polygon, query.anchor))
                {
                    inside = true;
                    break;
                }
            if (!inside)
                continue;
        }
        ++out.count;
        if (!out.best || target.score > out.best->score)
            out.best = &target;
    }
    return out;
}

/*======================== ROI 查询自由函数 (C 风格) ========================
 * 见 channel.h 结构体下方说明: 用一个 int idx 选区域, 单/多区域同一函数, 不用重载。
 *   idx==ROI_ALL → 所有区域(并集; 无区域=整帧); idx>=0 → 第 idx 区; 其它 → 无此区域=0。 */

int roi_find(const ChannelContext *ctx, const char *name)
{
    if (!ctx || !ctx->rois || !name)
        return ROI_NONE;
    std::string s(name);
    for (int i = 0; i < static_cast<int>(ctx->rois->size()); ++i)
        if ((*ctx->rois)[i].name == s)
            return i;
    return ROI_NONE;
}

int roi_contains(const ChannelContext *ctx, const cv::Rect &box, int idx)
{
    if (!ctx)
        return 0;
    if (idx == ROI_FRAME)
        return 1;
    if (idx == ROI_ALL) /* 所有区域 */
    {
        if (!ctx->rois || ctx->rois->empty())
            return 1; /* 没画区域 → 不设限 */
        for (const auto &zone : *ctx->rois)
            if (box_in_polygon(box, zone.polygon, TargetAnchor::Center))
                return 1;
        return 0;
    }
    if (idx < 0)
        return 0;                                                  /* ROI_NONE / 非法 */
    const std::vector<cv::Point> *poly = ctx->roi_polygon_at(idx); /* 指定区域 */
    return poly && box_in_polygon(box, *poly, TargetAnchor::Center) ? 1 : 0;
}

TargetQueryResult ChannelContext::query_targets(const TargetQuery &query) const
{
    return vision::query_targets(results, rois, query, inference_valid && infer_enabled);
}

int ChannelContext::roi_count() const
{
    return rois ? static_cast<int>(rois->size()) : 0;
}

const RoiZone *ChannelContext::roi_at(int idx) const
{
    return (rois && idx >= 0 && idx < static_cast<int>(rois->size())) ? &(*rois)[idx] : nullptr;
}

const std::vector<cv::Point> *ChannelContext::roi_polygon_at(int idx) const
{
    const RoiZone *z = roi_at(idx);
    return z ? &z->polygon : nullptr;
}

const RoiZone *ChannelContext::roi_by_name(const char *name) const
{
    if (!rois || !name)
        return nullptr;
    std::string s(name);
    for (const auto &z : *rois)
        if (z.name == s)
            return &z;
    return nullptr;
}
