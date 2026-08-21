/**
 * @file channel_logic.h
 * @brief 通道自定义业务逻辑接口 — C-style
 *
 * 架构说明:
 * - 跟踪器 (Tracker) 已移至 analyzer.cpp 全局执行
 * - 此模块仅用于用户自定义业务扩展
 *
 * 扩展方式 (每种逻辑一个独立模块目录, 自注册):
 *   1. 新建 src/logic/modules/logic_xxx/logic.cpp 和 logic.json
 *      logic.cpp 顶部 #include "logic/core/logic_common.h"
 *   2. 实现 static void logic_xxx(ChannelContext* ctx)
 *   3. 文件末尾注册: REGISTER_LOGIC(logic_xxx);
 *      需要像素时直接调用 ctx->model_frame() / ctx->source_frame()，框架会惰性取帧。
 *   4. 需要该后处理时，在 config.json 中把对应通道的 "logic" 字段设为 "logic_xxx"；
 *      不写或留空则只运行视频/模型管线，不执行任何业务模块。
 *
 * 删除一个逻辑: 直接删掉对应的模块目录即可 —— src/logic 下的 C++ 源文件由 CMake
 * 递归收集编译, 自注册和模块元数据也随之消失, 不牵连其它模块。
 */
#pragma once

#include "analyzer/algoProcess.h"
#include "config/config.h"
#include "logic_outputs.h"
#include <cstdint>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

/* 前置声明 */
struct ChannelFrameSnapshot;

/*======================== Web/外部通道动作 ========================*/
/**
 * Web按钮只传递通用的“动作名 + JSON参数”，具体含义由当前通道logic解释。
 * 动作不会在Socket线程直接执行，而是在该通道下一次logic调用前执行，因此可安全访问
 * ctx->state / frame / results，不会和逐帧业务逻辑并发修改状态。
 */
struct ChannelAction
{
    std::string request_id;
    std::string name;
    std::string payload_json = "{}";
    std::string logic_name; /* 入队时对应的logic，热切换后不误投给另一个logic */
    uint64_t received_unix_ms = 0;
};

struct ChannelActionResult
{
    // 这次按钮的请求是否被处理
    bool handled = false;
    // 对处理结果的文字说明，默认是空字符串
    std::string message;
};

/*======================== ROI 区域 (一个通道可配置多个) ========================*/
/**
 * 一个 ROI 区域 = 区域名 + 多边形顶点。顶点坐标系 = 模型输入尺寸(通常 640×640),
 * 与 ctx->results[].box 完全一致 —— 逻辑里可直接 cv::pointPolygonTest, 无需再缩放。
 *
 * 通道逻辑通过 ctx->rois (全部区域) 或 ctx->roi_by_name("xxx") / ctx->roi_polygon_at(i)
 * 等便捷方法访问本通道的各个区域。
 */
struct RoiZone
{
    std::string name;               /* 区域名(可空), 如 "entrance"/"exit"; 供逻辑按名取用 */
    std::vector<cv::Point> polygon; /* 顶点, 模型输入坐标系; >=3 个点才算有效区域 */
};

/*======================== 绘制指令 ========================*/
struct DrawCommand
{
    enum Type
    {
        RECT,
        CIRCLE,
        LINE,
        TEXT,
        POLYLINE,
        POLY_FILLED
    } type;

    enum Target : uint8_t
    {
        DISPLAY = 0x01,
        IMAGE = 0x02,
        VIDEO = 0x04,
        MEDIA = IMAGE | VIDEO,
        ALL = DISPLAY | IMAGE | VIDEO
    };
    uint8_t target = ALL;

    cv::Rect rect;
    cv::Point center;
    int radius = 0;
    cv::Point pt1, pt2;
    std::vector<cv::Point> points; /* POLYLINE: 折线顶点(模型坐标系) */
    bool closed = false;           /* POLYLINE: 是否闭合 */
    double alpha = 1.0;            /* 透明度 0~1, <1 半透明叠加(RECT/CIRCLE/POLYLINE/POLY_FILLED 均支持) */
    std::string text;
    cv::Point text_pos;
    double font_scale = 0.6;

    cv::Scalar color = cv::Scalar(0, 255, 0);
    int thickness = 2;
};

/*======================== render_overlays 参数包 ========================*/
struct RenderParams
{
    int chnId = 0;
    int inputW = 0, inputH = 0;
    float disp_fps = 0.0f;
    float infer_fps = 0.0f;
    int64_t result_age_ms = 0;
    int64_t result_frame_id = 0; /* 分割叠加缓存版本；同一推理结果可跨多个显示帧复用 */
    int show_fps = 1;
    uint8_t target_mask = DrawCommand::DISPLAY;
    bool show_system_overlays = true; /* ROI、检测框、姿态、分割 */
    bool show_custom_overlays = true; /* logic 的 draw_* 指令 */

    /* 本通道全部 ROI 区域(顶点均为模型输入坐标系); render_overlays 按 inputW/inputH 缩放后逐个绘制。 */
    const std::vector<RoiZone> *roi_zones = nullptr;
    const std::vector<AlgoResult> *results = nullptr;
    const std::vector<DrawCommand> *draw_cmds = nullptr;
};

/*======================== 帧时间 (年月日时分秒, 由 ctx->datetime() 拆出) ========================*/
struct FrameTime
{
    int year;   /* 如 2026 */
    int month;  /* 1~12 */
    int day;    /* 1~31 */
    int hour;   /* 0~23 */
    int minute; /* 0~59 */
    int second; /* 0~59 */
    int millis; /* 0~999 */
};

/*======================== 通道业务上下文  ========================*/
// 自定义的算法逻辑变量请勿加入本结构体中
// 若要定义web端可修改的变量,请在对应的logic.json中添加

/* 前置声明逻辑函数类型 */
struct ChannelContext;
class LogicParameterSet;
typedef void (*ChannelLogicFunc)(struct ChannelContext *ctx);
/* action 是仅在本次 handler 调用期间有效的只读借用指针；业务代码应先检查非空，不得跨帧保存。 */
typedef ChannelActionResult (*ChannelActionFunc)(struct ChannelContext *ctx, const ChannelAction *action);
typedef const cv::Mat *(*ChannelFrameGetter)(void *opaque);

struct ChannelContext
{
    /* ---- 唯一通道身份：config.channels[].id ---- */
    int chnId = -1;

    /* ---- 当前帧数据 ---- */
    /* 原始视频分辨率(摄像头/视频源解码出的真实尺寸, 如 1920×1080)。
     * 与 model_frame() 的区别: model_frame() 是缩放后的模型输入尺寸；下面是视频源真实宽高。
     * 首帧解码前可能为 0, 逻辑里用前可自行判一下 > 0。 */
    int src_width = 0;
    int src_height = 0;

    /* ---- 当前视频帧（惰性获取）----
     * model_frame(): 模型输入尺寸 BGR，坐标与 results/ROI 一致。
     * source_frame(): 原始视频分辨率 BGR，保留 src_width×src_height。
     * 每个函数只在本帧第一次调用时转换，之后复用同一份不可变缓存；完全不调用就没有转换开销。
     * 推理与非推理通道使用相同接口。返回对象只读，业务代码不得修改或跨帧保存指针；
     * 如需异步持有或修改，请显式 clone()。取帧失败返回 nullptr。 */
    const cv::Mat *model_frame() const;
    const cv::Mat *source_frame() const;

    /* 框架内部的惰性取帧绑定，业务 logic 不直接访问。 */
    ChannelFrameGetter model_frame_getter = nullptr;
    ChannelFrameGetter source_frame_getter = nullptr;
    void *frame_getter_opaque = nullptr;
    // 帧号
    int64_t frame_id;
    /* 近似系统开机后运行的毫秒数 */
    uint64_t timestamp_ms;
    /* Unix epoch 毫秒(UTC 基准, 即本业务帧进入分析管线时的墙钟): 配 time_hms()/time_str() */
    uint64_t unix_ms = 0;
    // 当前一帧与上一帧的间隔(毫秒)
    float dt_ms;
    // 推理结果
    std::vector<AlgoResult> *results;

    /* ---- 配置 (只读) ---- */
    const ChannelConfig *config;

    /* ---- 当前 logic 的专有参数（启动/热重载时已按模块 Schema 解析并补默认值） ---- */
    const LogicParameterSet *logic_parameters = nullptr;
    bool has_param(const char *key) const;
    float param_float(const char *key) const;
    int64_t param_int(const char *key) const;
    bool param_bool(const char *key) const;
    std::string param_string(const char *key) const;
    std::string param_json(const char *key) const;

    /* ---- 向全局 logic 发布同帧业务变量 ----
     * outputs 每帧重新创建并与 frame/results 一起原子发布。
     * 请在 logic.json 的 outputs[] 中声明相同的 key/type，便于画布展示数据契约。 */
    LogicOutputSet *outputs = nullptr;
    void publish_string(const char *key, const std::string &value) const;
    void publish_number(const char *key, double value) const;
    void publish_int(const char *key, int64_t value) const;
    void publish_bool(const char *key, bool value) const;
    void publish_json(const char *key, const std::string &json) const;

    /* ---- ROI (已缩放到模型输入坐标系) ----
     * rois 是本通道全部 ROI 区域；单个区域用 roi_polygon_at()/roi_by_name() 获取。 */
    const std::vector<RoiZone> *rois = nullptr;

    /* ---- 本帧绘制指令输出 ----
     * 框架在每次 logic 调用前创建并绑定；draw_text/draw_rect 等辅助函数会向其中
     * push DrawCommand，logic 返回后再由显示/图片/视频出口按 Target 延迟渲染。
     * 业务代码通常不直接操作，也绝不能缓存该指针跨帧使用。 */
    std::vector<DrawCommand> *draw_cmds = nullptr;

    /* ---- 显示输出（只影响当前通道的视频窗口）----
     * replace_display_frame(frame): 直接把处理后的图片作为本帧显示底图。
     * - 接受任意分辨率的 CV_8UC1 灰度图、CV_8UC3 BGR 图或 CV_8UC4 BGRA 图；
     * - 灰度/BGRA 会在此处一次性转成显示需要的 BGR，BGR 不深拷贝像素；
     * - 显示管线负责缩放到通道窗口，检测框、ROI、draw_* 指令仍会继续叠加；
     * - 传入 Mat 后不要再修改其像素；如必须继续修改，请传 frame.clone()。
     * 返回 false 表示图片为空、类型不支持，或当前上下文没有显示输出绑定。 */
    bool replace_display_frame(cv::Mat frame);

    /* ---- 可写模型尺寸显示画布 ----
     * 想"拿到显示画面 → 自由改像素 → 再显示"时调 display_canvas():
     * 返回一张可写的 640×640 BGR 图(首次调用 = 当前帧副本)，随意 cv:: 处理/贴图/写字；
     * 调用即表示"本帧用这张图当显示底图"。不调用则显示走原实时采集帧，行为不变。
     * 注意: 只改"显示"; 推理/上报仍用 model_frame()。draw_cmds(含中文 draw_text)仍叠加在它上面。*/
    cv::Mat *canvas = nullptr;   /* 两种显示接口共用的本帧输出缓冲 */
    bool *show_canvas = nullptr; /* 任一显示接口成功调用后置 true */
    cv::Mat &display_canvas();   /* 取可写显示画布并标记启用(见上) */

  /* ---- 跨帧持久化状态 ---- */
  // state是指向 std::shared_ptr<void> 对象的普通指针。
  // ctx->state：外层指针
  // *(ctx->state)：外层指针指向的 shared_ptr 对象
  // ctx->state->get()：shared_ptr 管理的原始 void* 指针
  std::shared_ptr<void>* state;

    /* ---- 是否开启推理 ---- */
    int infer_enabled;

    /* ---- 实时 fps ---- */
    float infer_fps;
    float disp_fps;

    /* ===== 整帧目标查询 (本通道) =====
     * 只看整帧、不分 ROI。按 ROI 查询(单/多区域统一)用本文件结构体下方的
     * C 风格自由函数 roi_contains / roi_has_target / roi_count_target(传 ctx 指针)。 */
    int has_target(const char *label) const;   /* 整帧: 是否有 label 类目标 */
    int target_count(const char *label) const; /* 整帧: label 类目标数量 */

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

    /* 框中心落在第几个区域(取首个命中); 都不在 / 无区域 → ROI_NONE
     * (ROI_NONE 而非 -1: 这样把返回值直接回传给 roi_contains 等自由函数也不会被误当成 ROI_ALL) */
    int roi_index_of(const cv::Rect &box) const;

    /* 本帧墙钟时间(unix_ms 按本地时区格式化) */
    std::string time_hms() const; /* "HH:MM:SS" —— 查看时间用这个 */
    std::string time_str() const; /* "YYYY-MM-DD HH:MM:SS" —— 上报/记录用 */
    FrameTime datetime() const; /* 拆成年月日时分秒独立 int(见 FrameTime), 不是字符串, 而是结构体元素 */

    RenderParams render_params(int64_t result_age_ms = 0) const;

    /* ===== 跨通道安全取数 (本通道 或 任意其它通道) =====
     *
     * get_channel_frame_snapshot(ch, out) 在一把 chn_mtx 锁内原子读出该通道的
     * frame + results + outputs + 绘制指令和发布元信息。frame 若存在则与 results 必定同帧；
     * 只有调用该带图快照接口时，框架才会惰性生成目标通道的模型尺寸图，
     * 返回后不持锁。失败会明确返回 false，不用空对象猜测通道是否存在。
     *
     * 典型用法 (在 channel logic 或 global logic 中):
     *   ChannelFrameSnapshot s;
     *   if (ctx->get_channel_frame_snapshot(2, &s) && s.logic.publication_age_ms < 500) {
     *       for (auto &r : s.results) { ... r.box, r.score, r.label ... }
     *   }
     * 本通道的当帧数据直接用 ctx->model_frame() / ctx->results / ctx->frame_id 即可。 */
    bool get_channel_frame_snapshot(int configuredId, ChannelFrameSnapshot *out) const;
    std::string get_channel_logic_name(int configuredId) const;
    int channel_has_logic(int configuredId, const char *logicName) const;
};

/*======================== ROI 便捷查询 (C 风格自由函数, 传 ctx 指针) ========================
 * 与 ctx->roi_count()/roi_at()/roi_index_of() 等成员函数的区别主要是 API 形式：
 * 成员函数隐式接收 this；下面的组合查询显式接收 ctx，能先判空，并用统一 idx
 * 处理 ROI_ALL/具体区域/ROI_NONE。“C 风格”不表示这些 C++ 类型接口具有 C ABI。
 * 不用重载/默认参: 用一个 int idx 选区域 —— 单区域、多区域同一个函数。
 *   idx == ROI_ALL       → 所有区域(并集; 没画区域=整帧, 不设限);
 *   idx >= 0             → 仅第 idx 个区域;
 *   其它(ROI_NONE/非法)  → 无此区域, 返回 0。
 * 按名字查: 先用 roi_find(ctx, "名字") 拿到序号再传入 —— 名字不存在返回 ROI_NONE,
 *           故绝不会被误当成 ROI_ALL。框中心落在第几个区域用 ctx->roi_index_of(box)。 */
enum
{
    ROI_ALL = -1,
    ROI_NONE = -2
};

// 判断检测框 box 是否属于编号为 idx 的 ROI
int roi_contains(const ChannelContext *ctx, const cv::Rect &box, int idx);

// 判断编号为 idx 的 ROI 中是否存在类别名称为 label 的检测目标
int roi_has_target(const ChannelContext *ctx, const char *label, int idx);

// 统计编号为 idx 的 ROI 中，类别名称等于 label 的检测目标数量
int roi_count_target(const ChannelContext *ctx, const char *label, int idx);

// 根据 ROI 名称 name 查询该 ROI 的编号
int roi_find(const ChannelContext *ctx, const char *name); /* 名字→序号; 找不到=ROI_NONE */

/*======================== 绘制辅助函数 ========================*/
/* 矩形/圆: thickness=-1(负数) = 填充; alpha<1 = 半透明叠加(目标/画面可透出来, 适合高亮报警区)。
 * 例: draw_rect(ctx, zone, 红, -1, 0.3)  → 半透明红色块盖住 zone, 区域内的人仍看得见。 */
void draw_rect(ChannelContext *ctx, const cv::Rect &rect, const cv::Scalar &color = cv::Scalar(0, 255, 0),
               int thickness = 2, double alpha = 1.0, DrawCommand::Target target = DrawCommand::ALL);

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius,
                 const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2, double alpha = 1.0,
                 DrawCommand::Target target = DrawCommand::ALL);

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2,
               const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
               DrawCommand::Target target = DrawCommand::ALL);

/* thickness = 加粗级别: <=1 普通填充字(默认外观); >=2 越大越粗(在填充字上叠同色描边来加粗)。
 * 报警大字想更醒目就调大 thickness, 如 draw_text(ctx,"报警",pos,红,1.0,4)。 */
/* target 可精确选择 DISPLAY / IMAGE / VIDEO；MEDIA 表示图片+视频，ALL 表示三者。 */
void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos,
               const cv::Scalar &color = cv::Scalar(255, 255, 255), double font_scale = 0.6, int thickness = 1,
               DrawCommand::Target target = DrawCommand::ALL);

/* 折线: 把一串点连成线; alpha<1 时半透明叠加(可让画面/手透过来, 看着更清楚)。
 * 比逐段 draw_line 更高效(一条指令), 且自交叠处不会因半透明而叠暗。 */
void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points,
                   const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2, double alpha = 1.0,
                   bool closed = false, DrawCommand::Target target = DrawCommand::ALL);

/* 填充多边形(实心色块); alpha<1 半透明叠加 —— 给一块 ROI/区域铺半透明底色高亮最常用。
 * 顶点为模型输入坐标系(与 ROI/检测框同坐标系); 少于 3 个点不绘制。
 * 例: draw_poly_filled(ctx, *ctx->roi_polygon_at(0), 红, 0.3)  → 把首个 ROI 铺成半透明红。 */
void draw_poly_filled(ChannelContext *ctx, const std::vector<cv::Point> &points,
                      const cv::Scalar &color = cv::Scalar(0, 255, 0), double alpha = 0.3,
                      DrawCommand::Target target = DrawCommand::ALL);

/*======================== 逻辑分发表接口 ========================*/
#define MAX_LOGIC_FUNCS 64

struct LogicEntry
{
    const char *name;
    ChannelLogicFunc func;
};

ChannelLogicFunc channel_logic_get(const char *name);
ChannelActionFunc channel_logic_action_get(const char *name);
/** 返回当前二进制实际注册的通道逻辑名，供 --list-logics 等只读能力探测使用。 */
std::vector<std::string> channel_logic_names();

/** @brief 注册一个 logic 到分发表 (同名则覆盖)。一般不直接调用, 用 REGISTER_LOGIC 宏。 */
void register_logic(const char *name, ChannelLogicFunc func);
void register_logic_action(const char *name, ChannelActionFunc func);

/*======================== 自注册辅助 (推荐用法) ========================*/
/**
 * 在某个 logic 的 .cpp 文件末尾写一行:
 *     REGISTER_LOGIC(logic_xxx);
 * 若代码需要图像，直接调用 ctx->model_frame() / ctx->source_frame()；无需在注册处预先声明。
 * 即可在 main() 之前(静态初始化阶段)把该 logic 自动注册进分发表 ——
 * 原理是构造一个文件作用域的静态对象, 其构造函数调用 register_logic。
 * 宏会把函数标识符自动字符串化；该函数名同时作为 config/Web/外部 API
 * 使用的唯一 logic ID，不再手写第二份注册字符串。
 *
 * 好处: 新增一个 logic 只需新增一个 .cpp 文件; 删除一个 logic 只需删掉对应 .cpp 文件,
 *       无需改动 channel_logic.cpp 或任何其它文件 —— 耦合最低。
 */
struct LogicRegistrar
{
    LogicRegistrar(const char *name, ChannelLogicFunc func)
    {
        register_logic(name, func);
    }
};
#define REGISTER_LOGIC(func) static const LogicRegistrar _logic_reg_##func(#func, func)

struct LogicActionRegistrar
{
    LogicActionRegistrar(const char *name, ChannelActionFunc func)
    {
        register_logic_action(name, func);
    }
};
#define REGISTER_LOGIC_ACTION(logic_func, func)                                                                        \
    static const LogicActionRegistrar _logic_action_reg_##func(#logic_func, func)
