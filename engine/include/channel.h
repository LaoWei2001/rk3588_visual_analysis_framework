/** @file channel.h
 * @brief 通道业务入口：帧、目标、ROI、参数、跨帧状态和业务变量。只在框架回调期间使用上下文。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <actions.h>
#include <config_types.h>
#include <drawing.h>
#include <memory>
#include <outputs.h>
#include <snapshot.h>
#include <types.h>
#include <utility>

/*======================== 通道业务上下文  ========================*/
// 自定义的算法逻辑变量请勿加入本结构体中
// 若要定义web端可修改的变量,请在对应的logic.json中添加

/* 前置声明逻辑函数类型 */
struct ChannelContext;
class LogicParameterSet;
typedef void (*ChannelLogicFunc)(struct ChannelContext *ctx);
/* action 是仅在本次 handler 调用期间有效的只读借用指针；业务代码应先检查非空，不得跨帧保存。 */
typedef LogicActionResult (*ChannelLogicActionFunc)(struct ChannelContext *ctx, const LogicAction *action);
typedef const cv::Mat *(*ChannelFrameGetter)(void *opaque);

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
    Center,       /* 框中心，整数宽/高的一半，与既有 ROI 查询一致。 */
    BottomCenter  /* 框底边中点，常用于判断脚点是否进入区域。 */
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
};

enum class TargetQueryStatus
{
    OK,
    INFERENCE_UNAVAILABLE,
    ROI_NOT_FOUND,
    INVALID_ROI,   /* 指定区域不足三个顶点；ROI_ALL 要求所有已配置区域均满足此条件。 */
    INVALID_QUERY  /* 非法置信度、区域选择值或判断点枚举。 */
};

struct TargetQueryResult
{
    TargetQueryStatus status = TargetQueryStatus::INFERENCE_UNAVAILABLE;
    int count = 0;
    /* 当前回调内借用的最高置信度目标；同分取原结果顺序中的第一个。
     * 不复制目标/图像，不得缓存指针跨帧使用或在使用前修改 results 容器。 */
    const AlgoResult *best = nullptr;

    bool valid() const { return status == TargetQueryStatus::OK; }
};

const char *target_query_status_name(TargetQueryStatus status);

struct ChannelContext
{
    /* 不触发取帧/转换。中心始终为 (320,320)，与实际模型输入尺寸独立。 */
    int business_width() const
    {
        return business_coordinates::WIDTH;
    }
    int business_height() const
    {
        return business_coordinates::HEIGHT;
    }
    cv::Size business_size() const
    {
        return business_coordinates::size();
    }
    cv::Point business_center() const
    {
        return business_coordinates::center();
    }

    /* ---- 唯一通道身份：config.channels[].id ---- */
    int chnId = -1;

    /* ---- 当前帧数据 ---- */
    /* 原始视频分辨率(摄像头/视频源解码出的真实尺寸, 如 1920×1080)。
     * 与 model_frame() 的区别: model_frame() 是固定 640×640 业务画布；下面是视频源真实宽高。
     * 首帧解码前可能为 0, 逻辑里用前可自行判一下 > 0。 */
    int src_width = 0;
    int src_height = 0;

    /* ---- 当前视频帧（惰性获取）----
     * model_frame()/business_frame(): 固定 640×640 完整画面 BGR，坐标与 results/ROI 一致。
     * source_frame(): 原始视频分辨率 BGR，保留 src_width×src_height。
     * 每个函数只在本帧第一次调用时转换，之后复用同一份不可变缓存；完全不调用就没有转换开销。
     * 推理与非推理通道使用相同接口。返回对象只读，业务代码不得修改或跨帧保存指针；
     * 如需异步持有或修改，请显式 clone()。取帧失败返回 nullptr。 */
    const cv::Mat *model_frame() const;
    /* 新代码可使用更明确的名称；与 model_frame() 共用同一惰性缓存，没有额外转换。 */
    const cv::Mat *business_frame() const
    {
        return model_frame();
    }
    const cv::Mat *source_frame() const;

  private:
    friend struct VisionContextAccess;
    /* 框架内部的惰性取帧绑定，业务 logic 不直接访问。 */
    ChannelFrameGetter model_frame_getter = nullptr;
    ChannelFrameGetter source_frame_getter = nullptr;
    void *frame_getter_opaque = nullptr;

  public:
    // 帧号
    int64_t frame_id;
    /* 近似系统开机后运行的毫秒数 */
    uint64_t timestamp_ms;
    /* Unix epoch 毫秒(UTC 基准, 即本业务帧进入分析管线时的墙钟): 配 time_hms()/time_str() */
    uint64_t unix_ms = 0;
    // 当前一帧与上一帧的间隔(毫秒)
    float dt_ms;
    // 推理结果
    std::vector<AlgoResult> *results = nullptr;

    /* ---- 配置 (只读) ---- */
    const ChannelConfig *config;

    /* ---- 当前 logic 的专有参数（启动/热重载时已按模块 Schema 解析并补默认值） ---- */
  private:
    const LogicParameterSet *logic_parameters = nullptr;

  public:
    bool has_param(const char *key) const;
    float param_float(const char *key) const;
    int64_t param_int(const char *key) const;
    bool param_bool(const char *key) const;
    std::string param_string(const char *key) const;
    std::string param_json(const char *key) const;

    /* ---- 向全局 logic 发布同帧业务变量 ----
     * outputs 每帧重新创建并与 frame/results 一起原子发布。
     * 请在 logic.json 的 outputs[] 中声明相同的 key/type，便于画布展示数据契约。 */
  private:
    LogicOutputSet *outputs = nullptr;

  public:
    void publish_string(const char *key, const std::string &value) const;
    void publish_number(const char *key, double value) const;
    void publish_int(const char *key, int64_t value) const;
    void publish_bool(const char *key, bool value) const;
    void publish_json(const char *key, const std::string &json) const;

    /* ---- ROI (已缩放到业务坐标系(640×640)) ----
     * rois 是本通道全部 ROI 区域；单个区域用 roi_polygon_at()/roi_by_name() 获取。 */
    const std::vector<RoiZone> *rois = nullptr;

    /* ---- 本帧绘制指令输出 ----
     * 框架在每次 logic 调用前创建并绑定；draw_text/draw_rect 等辅助函数会向其中
     * push DrawCommand，logic 返回后再由显示/图片/视频出口按 Target 延迟渲染。
     * 业务代码通常不直接操作，也绝不能缓存该指针跨帧使用。 */
  private:
    std::vector<DrawCommand> *draw_cmds = nullptr;

  public:
    /* ---- 显示输出（只影响当前通道的视频窗口）----
     * replace_display_frame(frame): 直接把处理后的图片作为本帧显示底图。
     * - 接受任意分辨率的 CV_8UC1 灰度图、CV_8UC3 BGR 图或 CV_8UC4 BGRA 图；
     * - 灰度/BGRA 会在此处一次性转成显示需要的 BGR，BGR 不深拷贝像素；
     * - 显示管线负责缩放到通道窗口，检测框、ROI、draw_* 指令仍会继续叠加；
     * - 传入 Mat 后不要再修改其像素；如必须继续修改，请传 frame.clone()。
     * 返回 false 表示图片为空、类型不支持，或当前上下文没有显示输出绑定。 */
    bool replace_display_frame(cv::Mat frame);

    /* ---- 可写业务尺寸显示画布 ----
     * 想"拿到显示画面 → 自由改像素 → 再显示"时调 display_canvas():
     * 返回一张可写的 640×640 BGR 图(首次调用 = 当前帧副本)，随意 cv:: 处理/贴图/写字；
     * 调用即表示"本帧用这张图当显示底图"。不调用则显示走原实时采集帧，行为不变。
     * 注意: 只改"显示"; 推理/上报仍用 model_frame()。draw_cmds(含中文 draw_text)仍叠加在它上面。*/
  private:
    cv::Mat *canvas = nullptr;   /* 两种显示接口共用的本帧输出缓冲 */
    bool *show_canvas = nullptr; /* 任一显示接口成功调用后置 true */
  public:
    cv::Mat &display_canvas(); /* 取可写显示画布并标记启用(见上) */

    /* ---- 跨帧持久化状态 ---- */
    // state是指向 std::shared_ptr<void> 对象的普通指针。
    // ctx->state：外层指针
    // *(ctx->state)：外层指针指向的 shared_ptr 对象
    // ctx->state->get()：shared_ptr 管理的原始 void* 指针
    std::shared_ptr<void> *state = nullptr;

    /** 获取本逻辑实例的跨帧状态，首次调用按 args 构造，后续调用复用。
     * 同一个模块的帧回调和 Action 必须使用相同的 T；不同实例的状态互相隔离。
     * 返回借用指针，仅在当前回调内使用；上下文未绑定状态时返回 nullptr。
     * 示例：auto *state = ctx->get_state<MyState>(); */
    template <typename T, typename... Args> T *get_state(Args &&...args) const
    {
        if (!state)
            return nullptr;
        if (!*state)
            *state = std::make_shared<T>(std::forward<Args>(args)...);
        return static_cast<T *>(state->get());
    }

    /* ---- 是否开启推理 ---- */
    int infer_enabled = 0;
    /* 成功发布的同帧推理结果为 true，空检测结果也可以有效。
     * 多模型可能仅部分子模型成功；不能据此认定每个子模型都有效。 */
    bool inference_valid = false;

    /* ---- 实时 fps ---- */
    float infer_fps;
    float disp_fps;

    /* ===== 整帧目标查询 (本通道) =====
     * 只看整帧、不分 ROI。按 ROI 查询(单/多区域统一)用本文件结构体下方的
     * C 风格自由函数 roi_contains / roi_has_target / roi_count_target(传 ctx 指针)。 */
    int has_target(const char *label) const;   /* 整帧: 是否有 label 类目标 */
    int target_count(const char *label) const; /* 整帧: label 类目标数量 */

    /** 一次遍历返回匹配数量和最高置信度目标，不构造匹配目标数组。
     * 先校验查询/区域，再检查 infer_enabled、inference_valid 和 results 是否绑定。
     * OK + count=0 是有效的空结果，失败时 count=0、best=nullptr。
     * 跳过非法置信度（非有限值或不在 [0,1]）及非正宽/高的检测框，区域包含边界。
     * 多模型下 OK 只表示本批结果有效，不保证每个子模型都成功，不去重不同模型的结果。
     * 旧 has_target/target_count/roi_* 的调用和有效性语义保持不变。 */
    TargetQueryResult query_targets(const TargetQuery &query = TargetQuery()) const;

    /* ===== ROI 区域访问 (本通道) =====
     * 一个通道可配置多个 ROI 区域(网页上各画一个、各取个名字)。下面这组按序号/名字取区域。
     * 所有多边形顶点都是业务坐标系(640×640), 与检测框同坐标系。 */

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

    /* ===== 跨通道安全取数 (本通道 或 任意其它通道) =====
     *
     * get_channel_frame_snapshot(ch, out) 在一把 chn_mtx 锁内原子读出该通道的
     * frame + results + outputs + 绘制指令和发布元信息。frame 若存在则与 results 必定同帧；
     * 只有调用该带图快照接口时，框架才会惰性生成目标通道的 640×640 业务图，
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
 *   idx == ROI_FRAME     → 整帧，忽略已配置区域;
 *   idx >= 0             → 仅第 idx 个区域;
 *   其它(ROI_NONE/非法)  → 无此区域, 返回 0。
 * 按名字查: 先用 roi_find(ctx, "名字") 拿到序号再传入 —— 名字不存在返回 ROI_NONE,
 *           故绝不会被误当成 ROI_ALL。框中心落在第几个区域用 ctx->roi_index_of(box)。 */
// 判断检测框 box 是否属于编号为 idx 的 ROI
int roi_contains(const ChannelContext *ctx, const cv::Rect &box, int idx);

// 判断编号为 idx 的 ROI 中是否存在类别名称为 label 的检测目标
int roi_has_target(const ChannelContext *ctx, const char *label, int idx);

// 统计编号为 idx 的 ROI 中，类别名称等于 label 的检测目标数量
int roi_count_target(const ChannelContext *ctx, const char *label, int idx);

// 根据 ROI 名称 name 查询该 ROI 的编号
int roi_find(const ChannelContext *ctx, const char *name); /* 名字→序号; 找不到=ROI_NONE */

/*======================== 逻辑注册 ========================*/
/** @brief 注册一个 logic 到分发表 (同名则覆盖)。一般不直接调用, 用 REGISTER_LOGIC 宏。 */
void register_logic(const char *name, ChannelLogicFunc func);
void register_logic_action(const char *name, ChannelLogicActionFunc func);

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
    LogicActionRegistrar(const char *name, ChannelLogicActionFunc func)
    {
        register_logic_action(name, func);
    }
};
#define REGISTER_LOGIC_ACTION(logic_func, func)                                                                        \
    static const LogicActionRegistrar _logic_action_reg_##func(#logic_func, func)
