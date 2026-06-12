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

/* 逻辑注册已改为各 logic 文件自注册(REGISTER_LOGIC), 在 main() 之前即全部就绪。
 * 这两个函数保留空实现, 仅为兼容 analyzer_init/analyzer_deinit 的既有调用。
 * 切勿在此清零 g_logic_count 或清空 g_logic_registry: 静态期的自注册只发生一次,
 * 一旦清空便无法恢复, 会导致 analyzer 重启后所有通道退化为空逻辑。 */
void channel_logic_init(void) {}
void channel_logic_deinit(void) {}

ChannelLogicFunc channel_logic_get(const char *name)
{
    if (name)
        for (int i = 0; i < g_logic_count; ++i)
            if (g_logic_registry[i].name && strcmp(g_logic_registry[i].name, name) == 0)
                return g_logic_registry[i].func;
    return logic_null;
}
