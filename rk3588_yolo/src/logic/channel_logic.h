/**
 * @file channel_logic.h
 * @brief 通道自定义业务逻辑接口 — C-style
 *
 * 架构说明:
 * - 跟踪器 (Tracker) 已移至 analyzer.cpp 全局执行
 * - 此模块仅用于用户自定义业务扩展
 *
 * 扩展方式 (每个逻辑一个独立 .cpp 文件, 自注册):
 *   1. 新建 src/logic/logic_xxx.cpp, 顶部 #include "logic_common.h"
 *   2. 实现 static void logic_xxx(ChannelContext* ctx)
 *   3. 文件末尾写一行: REGISTER_LOGIC("logic_xxx", logic_xxx);  // 自动注册, 无需改动其它文件
 *   4. 在 config.json 中把对应通道的 "logic" 字段设为 "logic_xxx"
 *
 * 删除一个逻辑: 直接删掉对应的 logic_xxx.cpp 文件即可 —— src/logic 下的 .cpp 由 CMake
 * (aux_source_directory) 自动收集编译, 自注册也随之消失, 不牵连任何其它文件。
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

/*======================== 帧时间 (年月日时分秒, 由 ctx->datetime() 拆出) ========================*/
struct FrameTime
{
    int year;    /* 如 2026 */
    int month;   /* 1~12 */
    int day;     /* 1~31 */
    int hour;    /* 0~23 */
    int minute;  /* 0~59 */
    int second;  /* 0~59 */
    int millis;  /* 0~999 */
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
    uint64_t timestamp_ms;                   /* 单调时钟(ms): 只用于算间隔, 不是日历时间 */
    uint64_t unix_ms = 0;                    /* Unix epoch 毫秒(UTC 基准, 即本帧墙钟; 三源统一): /1000 可得到秒 */
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

    /* ===== 整帧目标查询 (本通道) =====
     * 只看整帧、不分 ROI。按 ROI 查询(单/多区域统一)用本文件结构体下方的
     * C 风格自由函数 roi_contains / roi_has_target / roi_count_target(传 ctx 指针)。 */
    int has_target(const char *label) const;     /* 整帧: 是否有 label 类目标 */
    int target_count(const char *label) const;   /* 整帧: label 类目标数量 */

    /* ===== ROI 区域访问 (本通道) =====
     * 一个通道可配置多个 ROI 区域(网页上各画一个、各取个名字)。下面这组按序号/名字取区域。
     * 所有多边形顶点都是模型输入坐标系, 与检测框同坐标系。 */

    /* 本通道有效 ROI 区域数量 */
    int roi_count() const;

    /* 第 idx 个区域(越界返回 nullptr) */
    const RoiZone *roi_at(int idx) const;

    /* 第 idx 个区域的多边形(越界返回 nullptr) */
    const std::vector<cv::Point> *roi_polygon_at(int idx) const;

    /* 第 idx 个区域的名字(越界或无名返回 ""，永不为 nullptr) */
    const char *roi_name_at(int idx) const;

    /* 按名字取区域(找不到返回 nullptr) */
    const RoiZone *roi_by_name(const char *name) const;

    /* 某框中心是否落在指定多边形内(多边形不足 3 点 → 视为"全屏", 返回 1) */
    static int point_box_in_poly(const std::vector<cv::Point> *poly, const cv::Rect &box);

    /* 框中心落在第几个区域(取首个命中); 都不在 / 无区域 → -1 */
    int roi_index_of(const cv::Rect &box) const;

    /* 本帧墙钟时间(unix_ms 按本地时区格式化) */
    std::string time_hms() const;   /* "HH:MM:SS" —— 查看时间用这个 */
    std::string time_str() const;   /* "YYYY-MM-DD HH:MM:SS" —— 上报/记录用 */
    FrameTime   datetime() const;   /* 拆成年月日时分秒独立 int(见 FrameTime), 不只是字符串 */

    cv::Mat snapshot() const;

    RenderParams render_params(int64_t result_age_ms = 0) const;

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

/*======================== ROI 查询 (C 风格自由函数, 传 ctx 指针) ========================
 * 不用重载/默认参: 用一个 int idx 选区域 —— 单区域、多区域同一个函数。
 *   idx == ROI_ALL       → 所有区域(并集; 没画区域=整帧, 不设限);
 *   idx >= 0             → 仅第 idx 个区域;
 *   其它(ROI_NONE/非法)  → 无此区域, 返回 0。
 * 按名字查: 先用 roi_find(ctx, "名字") 拿到序号再传入 —— 名字不存在返回 ROI_NONE,
 *           故绝不会被误当成 ROI_ALL。框中心落在第几个区域用 ctx->roi_index_of(box)。 */
enum { ROI_ALL = -1, ROI_NONE = -2 };

int roi_contains    (const ChannelContext *ctx, const cv::Rect &box, int idx);
int roi_has_target  (const ChannelContext *ctx, const char *label,   int idx);
int roi_count_target(const ChannelContext *ctx, const char *label,   int idx);
int roi_find        (const ChannelContext *ctx, const char *name);   /* 名字→序号; 找不到=ROI_NONE */

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

ChannelLogicFunc channel_logic_get(const char *name);

/** @brief 注册一个 logic 到分发表 (同名则覆盖)。一般不直接调用, 用 REGISTER_LOGIC 宏。 */
void register_logic(const char *name, ChannelLogicFunc func);

/*======================== 自注册辅助 (推荐用法) ========================*/
/**
 * 在某个 logic 的 .cpp 文件末尾写一行:
 *     REGISTER_LOGIC("logic_xxx", logic_xxx);
 * 即可在 main() 之前(静态初始化阶段)把该 logic 自动注册进分发表 ——
 * 原理是构造一个文件作用域的静态对象, 其构造函数调用 register_logic。
 *
 * 好处: 新增一个 logic 只需新增一个 .cpp 文件; 删除一个 logic 只需删掉对应 .cpp 文件,
 *       无需改动 channel_logic.cpp 或任何其它文件 —— 耦合最低。
 */
struct LogicRegistrar
{
    LogicRegistrar(const char *name, ChannelLogicFunc func) { register_logic(name, func); }
};
#define REGISTER_LOGIC(name_str, func) \
    static const LogicRegistrar _logic_reg_##func(name_str, func)
