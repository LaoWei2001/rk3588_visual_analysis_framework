/**
 * @file channel_logic.h
 * @brief 通道自定义业务逻辑接口 — C-style
 *
 * 架构说明:
 * - 跟踪器 (Tracker) 已移至 analyzer.cpp 全局执行
 * - 此模块仅用于用户自定义业务扩展
 *
 * 扩展方式:
 *   1. 在 channel_logic.cpp 中实现 static void logic_xxx(ChannelContext* ctx)
 *   2. 在 channel_logic_init() 中调用 register_logic("logic_xxx", logic_xxx) 注册
 *   3. 在 config.json 中将对应通道的 "logic" 字段设为 "logic_xxx"
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <opencv2/opencv.hpp>
#include "../config/config.h"
#include "../analyzer/algoProcess.h"

/* 前置声明 */
struct ChannelSnapshot;
struct RenderParams;

/*======================== ROI 区域 (一个通道可配置多个) ========================*/
/**
 * 一个 ROI 区域 = 区域名 + 多边形顶点。顶点坐标系 = 模型输入尺寸(通常 640×640),
 * 与 ctx->results[].box 完全一致 —— 逻辑里可直接 cv::pointPolygonTest, 无需再缩放。
 *
 * 通道逻辑通过 ctx->rois (全部区域) 或 ctx->roi_by_name("xxx") / ctx->roi_polygon_at(i)
 * 等便捷方法访问本通道的各个区域; ctx->roi 仍指向"第一个区域"以兼容老逻辑。
 */
struct RoiZone
{
    std::string            name;     /* 区域名(可空), 如 "entrance"/"exit"; 供逻辑按名取用 */
    std::vector<cv::Point> polygon;  /* 顶点, 模型输入坐标系; >=3 个点才算有效区域 */
};

/*======================== 绘制指令 ========================*/
struct DrawCommand
{
    enum Type { RECT, CIRCLE, LINE, TEXT, POLYLINE } type;

    enum Target : uint8_t
    {
        DISPLAY = 0x01,
        UPLOAD  = 0x02,
        ALL     = 0x03
    };
    uint8_t target = ALL;

    cv::Rect rect;
    cv::Point center;
    int radius = 0;
    cv::Point pt1, pt2;
    std::vector<cv::Point> points;       /* POLYLINE: 折线顶点(模型坐标系) */
    bool   closed = false;               /* POLYLINE: 是否闭合 */
    double alpha  = 1.0;                  /* 透明度 0~1, <1 半透明叠加(当前 POLYLINE 支持) */
    std::string text;
    cv::Point text_pos;
    double font_scale = 0.6;

    cv::Scalar color = cv::Scalar(0, 255, 0);
    int thickness = 2;
};

/*======================== render_overlays 参数包 ========================*/
struct RenderParams
{
    int     chnId         = 0;
    int     srcWidth      = 0, srcHeight = 0;
    int     inputW        = 0, inputH    = 0;
    float   disp_fps      = 0.0f;
    float   infer_fps     = 0.0f;
    int64_t result_age_ms = 0;
    int     show_fps      = 1;
    uint8_t target_mask   = DrawCommand::DISPLAY;

    /* 本通道全部 ROI 区域(顶点均为模型输入坐标系); render_overlays 按 inputW/inputH 缩放后逐个绘制。 */
    const std::vector<RoiZone>     *roi_zones = nullptr;
    const std::vector<AlgoResult>  *results   = nullptr;
    const std::vector<DrawCommand> *draw_cmds = nullptr;
};

/*======================== 通道业务上下文 (C-style) ========================*/

/* 前置声明逻辑函数类型 */
struct ChannelContext;
typedef void (*ChannelLogicFunc)(struct ChannelContext *ctx);

struct ChannelContext
{
    /* ---- 标识 ---- */
    int chnId;

    /* ---- 当帧数据 (指针代替引用, C-style) ---- */
    const cv::Mat *frame;                    /* BGR, 模型输入尺寸 */
    int64_t frame_id;
    uint64_t timestamp_ms;
    float dt_ms;
    std::vector<AlgoResult> *results;

    /* ---- 配置 (只读) ---- */
    const ChannelConfig *config;

    /* ---- ROI (已缩放到模型输入坐标系) ----
     * roi  : 兼容字段, 指向"第一个区域"的多边形(无区域时为 nullptr); 老逻辑继续用它即可。
     * rois : 本通道全部 ROI 区域(支持一个视频流配多个区域); 用下方便捷方法访问更省事。 */
    const std::vector<cv::Point> *roi  = nullptr;
    const std::vector<RoiZone>   *rois = nullptr;

    /* ---- 绘制指令输出 ---- */
    std::vector<DrawCommand> *draw_cmds;

    /* ---- 跨帧持久化状态 ---- */
    std::shared_ptr<void> *state;

    /* ---- 元信息 ---- */
    int infer_enabled;

    /* ---- 实时 fps ---- */
    float infer_fps;
    float disp_fps;

    /* ===== 便捷查询 (本通道) ===== */
    int has_target(const char *label) const
    {
        if (!results) return 0;
        std::string s(label);
        for (const auto &r : *results)
            if (r.label == s) return 1;
        return 0;
    }

    int has_target_in_roi(const char *label) const
    {
        if (!results) return 0;
        if (!roi || roi->empty()) return has_target(label);
        std::string s(label);
        for (const auto &r : *results)
        {
            if (r.label == s)
            {
                cv::Point center(r.box.x + r.box.width / 2, r.box.y + r.box.height / 2);
                if (cv::pointPolygonTest(*roi, center, false) >= 0) return 1;
            }
        }
        return 0;
    }

    int is_in_roi(const cv::Rect &box) const
    {
        if (!roi || roi->empty()) return 1;
        cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
        return (cv::pointPolygonTest(*roi, center, false) >= 0) ? 1 : 0;
    }

    int target_count(const char *label) const
    {
        if (!results) return 0;
        std::string s(label);
        int n = 0;
        for (const auto &r : *results)
            if (r.label == s) ++n;
        return n;
    }

    /* ===== 多 ROI 便捷查询 (本通道) =====
     * 一个通道可配置多个 ROI 区域(在网页 ROI 节点上各画一个、各取个名字)。
     * 下面这组方法让逻辑能按"序号"或"名字"取到某个区域、并统计该区域内的目标。
     * 所有多边形顶点都是模型输入坐标系, 与检测框同坐标系。 */

    /* 本通道有效 ROI 区域数量 */
    int roi_count() const { return rois ? static_cast<int>(rois->size()) : 0; }

    /* 第 idx 个区域(越界返回 nullptr) */
    const RoiZone *roi_at(int idx) const
    {
        return (rois && idx >= 0 && idx < static_cast<int>(rois->size())) ? &(*rois)[idx] : nullptr;
    }

    /* 第 idx 个区域的多边形(越界返回 nullptr) */
    const std::vector<cv::Point> *roi_polygon_at(int idx) const
    {
        const RoiZone *z = roi_at(idx);
        return z ? &z->polygon : nullptr;
    }

    /* 第 idx 个区域的名字(越界或无名返回 ""，永不为 nullptr) */
    const char *roi_name_at(int idx) const
    {
        const RoiZone *z = roi_at(idx);
        return z ? z->name.c_str() : "";
    }

    /* 按名字取区域(找不到返回 nullptr) */
    const RoiZone *roi_by_name(const char *name) const
    {
        if (!rois || !name) return nullptr;
        std::string s(name);
        for (const auto &z : *rois)
            if (z.name == s) return &z;
        return nullptr;
    }

    /* 某框中心是否落在指定多边形内(多边形不足 3 点 → 视为"全屏", 返回 1) */
    static int point_box_in_poly(const std::vector<cv::Point> *poly, const cv::Rect &box)
    {
        if (!poly || poly->size() < 3) return 1;
        cv::Point c(box.x + box.width / 2, box.y + box.height / 2);
        return cv::pointPolygonTest(*poly, c, false) >= 0 ? 1 : 0;
    }

    /* 某框中心是否落在第 idx 个区域内(区域不存在 → 0) */
    int is_in_roi_idx(const cv::Rect &box, int idx) const
    {
        const std::vector<cv::Point> *poly = roi_polygon_at(idx);
        return (poly && poly->size() >= 3) ? point_box_in_poly(poly, box) : 0;
    }

    /* 统计第 idx 个区域内某类别目标数量(区域不存在 → 0) */
    int target_count_in_roi(const char *label, int idx) const
    {
        const std::vector<cv::Point> *poly = roi_polygon_at(idx);
        if (!results || !poly || poly->size() < 3) return 0;
        std::string s(label);
        int n = 0;
        for (const auto &r : *results)
            if (r.label == s && point_box_in_poly(poly, r.box)) ++n;
        return n;
    }

    /* 统计名为 name 的区域内某类别目标数量(无此区域 → 0) */
    int target_count_in_roi_named(const char *label, const char *name) const
    {
        const RoiZone *z = roi_by_name(name);
        if (!results || !z || z->polygon.size() < 3) return 0;
        std::string s(label);
        int n = 0;
        for (const auto &r : *results)
            if (r.label == s && point_box_in_poly(&z->polygon, r.box)) ++n;
        return n;
    }

    /* 第 idx 个区域内是否有某类别目标 */
    int has_target_in_roi_idx(const char *label, int idx) const
    {
        return target_count_in_roi(label, idx) > 0;
    }

    cv::Mat snapshot() const { return frame ? frame->clone() : cv::Mat(); }

    RenderParams render_params(int64_t result_age_ms = 0) const
    {
        RenderParams p;
        p.chnId         = chnId;
        p.srcWidth      = frame ? frame->cols : 0;
        p.srcHeight     = frame ? frame->rows : 0;
        p.inputW        = p.srcWidth;
        p.inputH        = p.srcHeight;
        p.disp_fps      = disp_fps;
        p.infer_fps     = infer_fps;
        p.result_age_ms = result_age_ms;
        p.roi_zones     = rois;
        p.results       = results;
        p.draw_cmds     = draw_cmds;
        return p;
    }

    /* ===== 跨通道安全取数 (本通道 或 任意其它通道) =====
     *
     * get_channel_snapshot(ch) 是获取【任意通道】数据的唯一推荐入口:
     * 它在一把 chn_mtx 锁内原子读出该通道的 frame + results + logic_state + fps + age + frame_seq,
     * 因此 snapshot.frame 与 snapshot.results 必定来自【同一帧】(snapshot.frame_seq 即该帧序号),
     * 绝不会出现"帧是旧的、框是另一时刻"的错位。返回值是深拷贝快照, 持有期间随便用, 无需再加锁。
     *
     * 典型用法 (在 channel logic 或 global logic 中):
     *   ChannelSnapshot s = ctx->get_channel_snapshot(2);   // 取通道 2
     *   if (!s.frame.empty() && s.result_age_ms < 500) {    // 新鲜度自检
     *       for (auto &r : s.results) { ... r.box, r.score, r.label ... }  // frame 与 results 同帧
     *   }
     * 本通道的当帧数据直接用 ctx->frame / ctx->results / ctx->frame_id 即可 (同样保证同帧)。 */
    ChannelSnapshot get_channel_snapshot(int chnId) const;
    std::string get_channel_logic_name(int chnId) const;
    int channel_has_logic(int chnId, const char *logicName) const;
};

/*======================== 绘制辅助函数 ========================*/
void draw_rect(ChannelContext *ctx, const cv::Rect &rect,
               const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
               DrawCommand::Target target = DrawCommand::ALL);

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius,
                 const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
                 DrawCommand::Target target = DrawCommand::ALL);

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2,
               const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
               DrawCommand::Target target = DrawCommand::ALL);

void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos,
               const cv::Scalar &color = cv::Scalar(255, 255, 255),
               double font_scale = 0.6, int thickness = 1,
               DrawCommand::Target target = DrawCommand::ALL);

/* 折线: 把一串点连成线; alpha<1 时半透明叠加(可让画面/手透过来, 看着更清楚)。
 * 比逐段 draw_line 更高效(一条指令), 且自交叠处不会因半透明而叠暗。 */
void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points,
                   const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
                   double alpha = 1.0, bool closed = false,
                   DrawCommand::Target target = DrawCommand::ALL);

/*======================== 逻辑分发表接口 ========================*/
#define MAX_LOGIC_FUNCS  32

struct LogicEntry
{
    const char      *name;
    ChannelLogicFunc func;
};

void channel_logic_init(void);
void channel_logic_deinit(void);
ChannelLogicFunc channel_logic_get(const char *name);

/** @brief 注册自定义 logic (在 channel_logic_init 末尾调用).
 *  示例: register_logic("logic_mine", logic_mine); */
void register_logic(const char *name, ChannelLogicFunc func);
