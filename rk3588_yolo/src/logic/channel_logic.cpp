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
 * 新增逻辑: 复制一个 logic_xxx.cpp 即可 (src/logic 下的 .cpp 由 CMake
 * 自动收集编译), 无需改动本文件。 删除逻辑: 删掉对应的 logic_xxx.cpp 文件即可,
 * 不牵连任何其它文件。
 */

#include "logic_common.h"
#include <ctime>

/*======================== ChannelContext 跨通道方法实现
 * ========================*/
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

/*======================== ChannelContext 便捷查询方法实现
 * ======================== 这些方法原先内联在 channel_logic.h 的结构体定义里,
 * 现统一挪到此处。好处:
 *   - 头文件回归「纯 API 清单」, 一眼看清 ctx 能干啥;
 *   - 改任何函数体只需重编本文件, 不再波及 30+ 个 logic_*.cpp
 * (原先内联时全得重编)。 这些都是每帧级调用, 内部 string 比较 /
 * pointPolygonTest 远重于一次函数调用,
 * 因此不再跨编译单元内联也无可测量的性能影响。
 * 注意: 静态成员 point_box_in_poly 与带默认参数的 render_params 在此定义时,
 *       均不重复 static / 默认实参 (默认实参只写在头文件声明处)。 */

int ChannelContext::has_target(const char *label) const
{
    if (!results)
        return 0;
    std::string s(label);
    for (const auto &r : *results)
        if (r.label == s)
            return 1;
    return 0;
}

/*======================== ROI 查询自由函数 (C 风格) ========================
 * 见 channel_logic.h 结构体下方说明: 用一个 int idx 选区域, 单/多区域同一函数,
 * 不用重载。 idx==ROI_ALL → 所有区域(并集; 无区域=整帧); idx>=0 → 第 idx 区;
 * 其它 → 无此区域=0。 */

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
    if (idx == ROI_ALL) /* 所有区域 */
    {
        if (!ctx->rois || ctx->rois->empty())
            return 1; /* 没画区域 → 不设限 */
        return ctx->roi_index_of(box) >= 0 ? 1 : 0;
    }
    if (idx < 0)
        return 0;                                                  /* ROI_NONE / 非法 */
    const std::vector<cv::Point> *poly = ctx->roi_polygon_at(idx); /* 指定区域 */
    return (poly && poly->size() >= 3) ? ChannelContext::point_box_in_poly(poly, box) : 0;
}

int roi_has_target(const ChannelContext *ctx, const char *label, int idx)
{
    if (!ctx || !ctx->results)
        return 0;
    if (idx == ROI_ALL) /* 所有区域 */
    {
        if (!ctx->rois || ctx->rois->empty())
            return ctx->has_target(label); /* 无区域 → 整帧 */
        std::string s(label);
        for (const auto &r : *ctx->results)
            if (r.label == s && ctx->roi_index_of(r.box) >= 0)
                return 1;
        return 0;
    }
    if (idx < 0)
        return 0;                                                  /* ROI_NONE / 非法 */
    const std::vector<cv::Point> *poly = ctx->roi_polygon_at(idx); /* 指定区域 */
    if (!poly || poly->size() < 3)
        return 0;
    std::string s(label);
    for (const auto &r : *ctx->results)
        if (r.label == s && ChannelContext::point_box_in_poly(poly, r.box))
            return 1;
    return 0;
}

int roi_count_target(const ChannelContext *ctx, const char *label, int idx)
{
    if (!ctx || !ctx->results)
        return 0;
    if (idx == ROI_ALL) /* 所有区域(并集, 重叠不重复计) */
    {
        if (!ctx->rois || ctx->rois->empty())
            return ctx->target_count(label); /* 无区域 → 整帧 */
        std::string s(label);
        int n = 0;
        for (const auto &r : *ctx->results)
            if (r.label == s && ctx->roi_index_of(r.box) >= 0)
                ++n;
        return n;
    }
    if (idx < 0)
        return 0;                                                  /* ROI_NONE / 非法 */
    const std::vector<cv::Point> *poly = ctx->roi_polygon_at(idx); /* 指定区域 */
    if (!poly || poly->size() < 3)
        return 0;
    std::string s(label);
    int n = 0;
    for (const auto &r : *ctx->results)
        if (r.label == s && ChannelContext::point_box_in_poly(poly, r.box))
            ++n;
    return n;
}

int ChannelContext::target_count(const char *label) const
{
    if (!results)
        return 0;
    std::string s(label);
    int n = 0;
    for (const auto &r : *results)
        if (r.label == s)
            ++n;
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
    if (!rois || !name)
        return nullptr;
    std::string s(name);
    for (const auto &z : *rois)
        if (z.name == s)
            return &z;
    return nullptr;
}

int ChannelContext::point_box_in_poly(const std::vector<cv::Point> *poly, const cv::Rect &box)
{
    if (!poly || poly->size() < 3)
        return 1;
    cv::Point c(box.x + box.width / 2, box.y + box.height / 2);
    return cv::pointPolygonTest(*poly, c, false) >= 0 ? 1 : 0;
}

/*======================== roi_index_of —— ROI 区域定位辅助
 * ======================== 给一个框,
 * 返回它中心落在第几个区域(取首个命中)。roi_contains / roi_has_target /
 * roi_count_target 的"任一区域"分支都建立在它之上。 */

int ChannelContext::roi_index_of(const cv::Rect &box) const
{
    if (!rois)
        return ROI_NONE;
    cv::Point c(box.x + box.width / 2, box.y + box.height / 2);
    for (int i = 0; i < static_cast<int>(rois->size()); ++i)
    {
        const std::vector<cv::Point> &poly = (*rois)[i].polygon;
        if (poly.size() >= 3 && cv::pointPolygonTest(poly, c, false) >= 0)
            return i;
    }
    return ROI_NONE;
}

cv::Mat ChannelContext::snapshot() const
{
    return frame ? frame->clone() : cv::Mat();
}

/* 取可写显示画布:
 * 首次调用以当前帧为底克隆一张可写副本，并标记"本帧用它当显示底图"。 之后随意
 * cv:: 处理(滤镜/贴图/putText 等)；中文叠加用 draw_text(走 draw_cmds,
 * 会叠在它上面)。 */
cv::Mat &ChannelContext::display_canvas()
{
    if (canvas->empty() && frame && !frame->empty())
        *canvas = frame->clone();
    *show_canvas = true;
    return *canvas;
}

/* unix_ms(epoch 毫秒)→ 本地时区时间串。localtime_r 线程安全(logic 在 worker
 * 线程跑)。 */
std::string ChannelContext::time_hms() const
{
    time_t sec = static_cast<time_t>(unix_ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    return buf;
}

std::string ChannelContext::time_str() const
{
    time_t sec = static_cast<time_t>(unix_ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

FrameTime ChannelContext::datetime() const
{
    time_t sec = static_cast<time_t>(unix_ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    FrameTime t;
    t.year = tmv.tm_year + 1900;
    t.month = tmv.tm_mon + 1;
    t.day = tmv.tm_mday;
    t.hour = tmv.tm_hour;
    t.minute = tmv.tm_min;
    t.second = tmv.tm_sec;
    t.millis = static_cast<int>(unix_ms % 1000);
    return t;
}

RenderParams ChannelContext::render_params(int64_t result_age_ms) const
{
    RenderParams p;
    p.chnId = chnId;
    p.inputW = frame ? frame->cols : 0;
    p.inputH = frame ? frame->rows : 0;
    p.disp_fps = disp_fps;
    p.infer_fps = infer_fps;
    p.result_age_ms = result_age_ms;
    p.roi_zones = rois;
    p.results = results;
    p.draw_cmds = draw_cmds;
    return p;
}

/*======================== 绘制辅助函数实现 ========================*/
void draw_rect(ChannelContext *ctx, const cv::Rect &rect, const cv::Scalar &color, int thickness, double alpha,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::RECT;
    cmd.rect = rect;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.alpha = alpha;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius, const cv::Scalar &color, int thickness,
                 double alpha, DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::CIRCLE;
    cmd.center = center;
    cmd.radius = radius;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.alpha = alpha;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2, const cv::Scalar &color, int thickness,
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

void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos, const cv::Scalar &color, double font_scale,
               int thickness, DrawCommand::Target target)
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

void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points, const cv::Scalar &color, int thickness,
                   double alpha, bool closed, DrawCommand::Target target)
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

void draw_poly_filled(ChannelContext *ctx, const std::vector<cv::Point> &points, const cv::Scalar &color, double alpha,
                      DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds || points.size() < 3)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::POLY_FILLED;
    cmd.points = points;
    cmd.alpha = alpha;
    cmd.color = color;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

/*======================== 逻辑分发表 ========================*/
static LogicEntry g_logic_registry[MAX_LOGIC_FUNCS];
static int g_logic_count = 0;

/* channel_logic_get 找不到目标时的兜底: 空逻辑(不画不报)。
 * 注意它与用户可在 config 里选择的 "logic_default" 不同 —— 后者在
 * logic_default.cpp 中定义并自注册; 这里只是分发表查不到名字时的内部兜底,
 * 行为同样是"什么都不做"。 */
static void logic_null(ChannelContext *ctx)
{
    (void)ctx;
}

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
