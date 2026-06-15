/**
 * @file channel_logic.cpp
 * @brief 通道自定义业务逻辑 —— 框架核心 (C-style)
 *
 * 本文件只保留「框架」部分, 不含任何具体业务逻辑:
 *   - ChannelContext 跨通道方法实现
 *   - draw_* 绘制辅助函数
 *   - 逻辑分发表 (注册 / 查询)
 *
 * 各个具体业务逻辑已拆分为独立文件 src/logic/logic_xxx.cpp, 每个文件在末尾用
 *   REGISTER_LOGIC("logic_xxx", logic_xxx);
 * 自注册到分发表 —— 注册在 main() 之前(静态初始化阶段)完成。
 *
 * 新增逻辑: 复制一个 logic_xxx.cpp 即可 (src/logic 下的 .cpp 由 CMake 自动收集编译),
 *           无需改动本文件。
 * 删除逻辑: 删掉对应的 logic_xxx.cpp 文件即可, 不牵连任何其它文件。
 */

#include "logic_common.h"

/*======================== ChannelContext 跨通道方法实现 ========================*/
ChannelSnapshot ChannelContext::get_channel_snapshot(int chnId) const
{
    ChannelSnapshot out;
    app_ctrl_get_channel_snapshot(chnId, &out);
    return out;
}

std::string ChannelContext::get_channel_logic_name(int chnId) const
{
    return app_ctrl_get_logic_name(chnId);
}

int ChannelContext::channel_has_logic(int chnId, const char *logicName) const
{
    return app_ctrl_get_logic_name(chnId) == std::string(logicName) ? 1 : 0;
}

/*======================== ChannelContext 便捷查询方法实现 ========================
 * 这些方法原先内联在 channel_logic.h 的结构体定义里, 现统一挪到此处。好处:
 *   - 头文件回归「纯 API 清单」, 一眼看清 ctx 能干啥;
 *   - 改任何函数体只需重编本文件, 不再波及 30+ 个 logic_*.cpp (原先内联时全得重编)。
 * 这些都是每帧级调用, 内部 string 比较 / pointPolygonTest 远重于一次函数调用,
 * 因此不再跨编译单元内联也无可测量的性能影响。
 * 注意: 静态成员 point_box_in_poly 与带默认参数的 render_params 在此定义时,
 *       均不重复 static / 默认实参 (默认实参只写在头文件声明处)。 */

int ChannelContext::has_target(const char *label) const
{
    if (!results) return 0;
    std::string s(label);
    for (const auto &r : *results)
        if (r.label == s) return 1;
    return 0;
}

int ChannelContext::has_target_in_roi(const char *label) const
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

int ChannelContext::is_in_roi(const cv::Rect &box) const
{
    if (!roi || roi->empty()) return 1;
    cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
    return (cv::pointPolygonTest(*roi, center, false) >= 0) ? 1 : 0;
}

int ChannelContext::target_count(const char *label) const
{
    if (!results) return 0;
    std::string s(label);
    int n = 0;
    for (const auto &r : *results)
        if (r.label == s) ++n;
    return n;
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

const char *ChannelContext::roi_name_at(int idx) const
{
    const RoiZone *z = roi_at(idx);
    return z ? z->name.c_str() : "";
}

const RoiZone *ChannelContext::roi_by_name(const char *name) const
{
    if (!rois || !name) return nullptr;
    std::string s(name);
    for (const auto &z : *rois)
        if (z.name == s) return &z;
    return nullptr;
}

int ChannelContext::point_box_in_poly(const std::vector<cv::Point> *poly, const cv::Rect &box)
{
    if (!poly || poly->size() < 3) return 1;
    cv::Point c(box.x + box.width / 2, box.y + box.height / 2);
    return cv::pointPolygonTest(*poly, c, false) >= 0 ? 1 : 0;
}

int ChannelContext::is_in_roi_idx(const cv::Rect &box, int idx) const
{
    const std::vector<cv::Point> *poly = roi_polygon_at(idx);
    return (poly && poly->size() >= 3) ? point_box_in_poly(poly, box) : 0;
}

int ChannelContext::target_count_in_roi(const char *label, int idx) const
{
    const std::vector<cv::Point> *poly = roi_polygon_at(idx);
    if (!results || !poly || poly->size() < 3) return 0;
    std::string s(label);
    int n = 0;
    for (const auto &r : *results)
        if (r.label == s && point_box_in_poly(poly, r.box)) ++n;
    return n;
}

int ChannelContext::target_count_in_roi_named(const char *label, const char *name) const
{
    const RoiZone *z = roi_by_name(name);
    if (!results || !z || z->polygon.size() < 3) return 0;
    std::string s(label);
    int n = 0;
    for (const auto &r : *results)
        if (r.label == s && point_box_in_poly(&z->polygon, r.box)) ++n;
    return n;
}

int ChannelContext::has_target_in_roi_idx(const char *label, int idx) const
{
    return target_count_in_roi(label, idx) > 0;
}

cv::Mat ChannelContext::snapshot() const
{
    return frame ? frame->clone() : cv::Mat();
}

RenderParams ChannelContext::render_params(int64_t result_age_ms) const
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

/*======================== 绘制辅助函数实现 ========================*/
void draw_rect(ChannelContext *ctx, const cv::Rect &rect,
               const cv::Scalar &color, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::RECT;
    cmd.rect = rect;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius,
                 const cv::Scalar &color, int thickness,
                 DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::CIRCLE;
    cmd.center = center;
    cmd.radius = radius;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2,
               const cv::Scalar &color, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::LINE;
    cmd.pt1 = pt1;
    cmd.pt2 = pt2;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos,
               const cv::Scalar &color, double font_scale, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds || !text)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::TEXT;
    cmd.text = text;
    cmd.text_pos = pos;
    cmd.font_scale = font_scale;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points,
                   const cv::Scalar &color, int thickness,
                   double alpha, bool closed,
                   DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds || points.size() < 2)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::POLYLINE;
    cmd.points = points;
    cmd.closed = closed;
    cmd.alpha = alpha;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

/*======================== 逻辑分发表 ========================*/
static LogicEntry g_logic_registry[MAX_LOGIC_FUNCS];
static int        g_logic_count = 0;

/* channel_logic_get 找不到目标时的兜底: 空逻辑(不画不报)。
 * 注意它与用户可在 config 里选择的 "logic_default" 不同 —— 后者在 logic_default.cpp
 * 中定义并自注册; 这里只是分发表查不到名字时的内部兜底, 行为同样是"什么都不做"。 */
static void logic_null(ChannelContext *ctx) { (void)ctx; }

void register_logic(const char *name, ChannelLogicFunc func)
{
    if (!name || !func)
        return;
    /* 同名则覆盖(防自注册与历史手动注册重复, 也便于替换实现) */
    for (int i = 0; i < g_logic_count; ++i)
        if (g_logic_registry[i].name && strcmp(g_logic_registry[i].name, name) == 0)
        {
            g_logic_registry[i].func = func;
            return;
        }
    if (g_logic_count < MAX_LOGIC_FUNCS)
    {
        g_logic_registry[g_logic_count].name = name;
        g_logic_registry[g_logic_count].func = func;
        g_logic_count++;
    }
}

ChannelLogicFunc channel_logic_get(const char *name)
{
    if (name)
        for (int i = 0; i < g_logic_count; ++i)
            if (g_logic_registry[i].name && strcmp(g_logic_registry[i].name, name) == 0)
                return g_logic_registry[i].func;
    return logic_null;
}
