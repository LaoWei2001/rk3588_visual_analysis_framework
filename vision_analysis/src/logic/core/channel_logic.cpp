/**
 * @file channel_logic.cpp
 * @brief 通道自定义业务逻辑 —— 核心
 *
 * 本文件只保留「框架」部分, 不含任何具体业务逻辑:
 *   - ChannelContext 跨通道方法实现
 *   - draw_* 绘制辅助函数
 *   - 逻辑分发表 (注册 / 查询)
 *
 * 各个具体业务逻辑位于 src/logic/modules/logic_xxx/, logic.cpp 末尾用
 *   REGISTER_LOGIC(logic_xxx);
 * 自注册到分发表 —— 注册在 main() 之前(静态初始化阶段)完成。
 *
 * 新增逻辑: 新建一个模块目录及 logic.cpp/logic.json 即可
 *           (src/logic 下的 .cpp/.cc/.cxx 由 CMake 递归收集编译), 无需改动本文件。
 * 删除逻辑: 删掉对应模块目录即可, 不牵连其它模块。
 */

#include "logic_common.h"
#include "logic_parameters.h"
#include <ctime>
#include <utility>

/*======================== 模块专有参数访问 ========================*/
bool ChannelContext::has_param(const char *key) const
{
    return logic_parameters && logic_parameters->has(key);
}

float ChannelContext::param_float(const char *key) const
{
    return logic_parameters ? logic_parameters->get_float(key) : 0.0f;
}

int64_t ChannelContext::param_int(const char *key) const
{
    return logic_parameters ? logic_parameters->get_int(key) : 0;
}

bool ChannelContext::param_bool(const char *key) const
{
    return logic_parameters ? logic_parameters->get_bool(key) : false;
}

std::string ChannelContext::param_string(const char *key) const
{
    return logic_parameters ? logic_parameters->get_string(key) : std::string();
}

std::string ChannelContext::param_json(const char *key) const
{
    return logic_parameters ? logic_parameters->get_json(key) : std::string();
}

void ChannelContext::publish_string(const char *key, const std::string &value) const
{
    if (outputs)
        outputs->set_string(key, value);
}

void ChannelContext::publish_number(const char *key, double value) const
{
    if (outputs)
        outputs->set_number(key, value);
}

void ChannelContext::publish_int(const char *key, int64_t value) const
{
    if (outputs)
        outputs->set_int(key, value);
}

void ChannelContext::publish_bool(const char *key, bool value) const
{
    if (outputs)
        outputs->set_bool(key, value);
}

void ChannelContext::publish_json(const char *key, const std::string &json) const
{
    if (outputs)
        outputs->set_json(key, json);
}

/*======================== ChannelContext 跨通道方法实现 ========================*/
bool ChannelContext::get_channel_frame_snapshot(int configuredId, ChannelFrameSnapshot *out) const
{
    return out && app_ctrl_get_channel_frame_snapshot(configuredId, out) != 0;
}

std::string ChannelContext::get_channel_logic_name(int configuredId) const
{
    return app_ctrl_get_logic_name(configuredId);
}

int ChannelContext::channel_has_logic(int configuredId, const char *logicName) const
{
    return app_ctrl_get_logic_name(configuredId) == std::string(logicName) ? 1 : 0;
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
    if (!results)
        return 0;
    std::string s(label);
    for (const auto &r : *results)
        if (r.label == s)
            return 1;
    return 0;
}

/*======================== ROI 查询自由函数 (C 风格) ========================
 * 见 channel_logic.h 结构体下方说明: 用一个 int idx 选区域, 单/多区域同一函数, 不用重载。
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

/*======================== roi_index_of —— ROI 区域定位辅助 ========================
 * 给一个框, 返回它中心落在第几个区域(取首个命中)。roi_contains / roi_has_target /
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

const cv::Mat *ChannelContext::model_frame() const
{
    return model_frame_getter ? model_frame_getter(frame_getter_opaque) : nullptr;
}

const cv::Mat *ChannelContext::source_frame() const
{
    return source_frame_getter ? source_frame_getter(frame_getter_opaque) : nullptr;
}

bool ChannelContext::replace_display_frame(cv::Mat frame)
{
    if (!canvas || !show_canvas || frame.empty() || frame.depth() != CV_8U)
        return false;

    if (frame.channels() == 3)
    {
        /* cv::Mat 按引用计数持有像素；这里移动 Mat 头，不深拷贝整帧。 */
        *canvas = std::move(frame);
    }
    else if (frame.channels() == 1)
    {
        cv::cvtColor(frame, *canvas, cv::COLOR_GRAY2BGR);
    }
    else if (frame.channels() == 4)
    {
        cv::cvtColor(frame, *canvas, cv::COLOR_BGRA2BGR);
    }
    else
    {
        return false;
    }

    *show_canvas = !canvas->empty();
    return *show_canvas;
}

/* 取可写显示画布: 首次调用以当前帧为底克隆一张可写副本，并标记"本帧用它当显示底图"。
 * 之后随意 cv:: 处理(滤镜/贴图/putText 等)；中文叠加用 draw_text(走 draw_cmds, 会叠在它上面)。 */
cv::Mat &ChannelContext::display_canvas()
{
    const cv::Mat *frame = model_frame();
    if (canvas->empty() && frame && !frame->empty())
        *canvas = frame->clone();
    *show_canvas = true;
    return *canvas;
}

/* unix_ms(epoch 毫秒)→ 本地时区时间串。localtime_r 线程安全(logic 在 worker 线程跑)。 */
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
    const cv::Mat *frame = model_frame();
    p.chnId = chnId;
    p.inputW = frame ? frame->cols : 0;
    p.inputH = frame ? frame->rows : 0;
    p.disp_fps = disp_fps;
    p.infer_fps = infer_fps;
    p.result_age_ms = result_age_ms;
    p.result_frame_id = frame_id;
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

struct LogicActionEntry
{
    const char *name = nullptr;
    ChannelLogicActionFunc func = nullptr;
};
static LogicActionEntry g_action_registry[MAX_LOGIC_FUNCS];
static int g_action_count = 0;

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
    return nullptr;
}

std::vector<std::string> channel_logic_names()
{
    std::vector<std::string> names;
    names.reserve(g_logic_count);
    for (int i = 0; i < g_logic_count; ++i)
        if (g_logic_registry[i].name)
            names.emplace_back(g_logic_registry[i].name);
    std::sort(names.begin(), names.end());
    return names;
}

void register_logic_action(const char *name, ChannelLogicActionFunc func)
{
    if (!name || !func)
        return;
    for (int i = 0; i < g_action_count; ++i)
        if (g_action_registry[i].name && strcmp(g_action_registry[i].name, name) == 0)
        {
            g_action_registry[i].func = func;
            return;
        }
    if (g_action_count < MAX_LOGIC_FUNCS)
    {
        g_action_registry[g_action_count].name = name;
        g_action_registry[g_action_count].func = func;
        ++g_action_count;
    }
}

ChannelLogicActionFunc channel_logic_action_get(const char *name)
{
    if (name)
        for (int i = 0; i < g_action_count; ++i)
            if (g_action_registry[i].name && strcmp(g_action_registry[i].name, name) == 0)
                return g_action_registry[i].func;
    return nullptr;
}
