/** @file global.h
 * @brief 全局业务入口：读取有效通道输入、跨 tick 状态和跨通道证据。调度与线程由引擎管理。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <channel.h>
#include <events.h>
#include <map>

/**
 * @brief 全局 Logic 的业务输入视图。
 *
 * 只有已经发布、当前在线且仍然有效的通道才会由框架放入 input_count()/input_at() 返回的批次。本类型刻意不暴露
 * 发布时间和数据年龄；普通跨通道业务只读取公开变量，输入有效性由调度器统一负责。
 */
class ChannelInput
{
  public:
    /** @brief 创建空输入视图；channel_id 为 -1，读取失败。
     * 示例：
     * @code
     * ChannelInput empty;
     * bool present = empty.has("count");
     * @endcode
     */
    ChannelInput() = default;
    /** @brief 借用快照构建输入视图；不复制、不检查在线/时效，快照须保持存活。
     * 示例：
     * @code
     * ChannelLogicSnapshot snapshot;
     * ChannelInput input(&snapshot);
     * @endcode
     */
    explicit ChannelInput(const ChannelLogicSnapshot *snapshot) : snapshot_(snapshot)
    {
    }

    /** @brief 返回配置通道 ID；空视图返回 -1，ID 不是输入数组下标。
     * 示例：
     * @code
     * if (const auto *in = gctx->input_at(0))
     * {
     *     int id = in->channel_id();
     *     (void)id;
     * }
     * @endcode
     */
    int channel_id() const
    {
        return snapshot_ ? snapshot_->channel_id : -1;
    }

    /** @brief 检查输出字段是否存在；不判断类型，空 key 或空视图返回 false。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     bool exists = in->has("count");
     *     (void)exists;
     * }
     * @endcode
     */
    bool has(const char *key) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->has(key);
    }

    /** @brief 严格读取字符串；成功返回 true，缺失、类型不符或空 out 返回 false 且不改 out。
     * 示例：
     * @code
     * std::string value = "";
     * if (const auto *in = gctx->input(2))
     *     in->read_string("value", &value);
     * @endcode
     */
    bool read_string(const char *key, std::string *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_string(key, out);
    }

    /** @brief 严格读取 double 数值；成功返回 true，缺失、类型不符或空 out 返回 false 且不改 out。
     * 示例：
     * @code
     * double value = 0.0;
     * if (const auto *in = gctx->input(2))
     *     in->read_number("value", &value);
     * @endcode
     */
    bool read_number(const char *key, double *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_number(key, out);
    }

    /** @brief 严格读取 int64 整数；成功返回 true，缺失、类型不符或空 out 返回 false 且不改 out。
     * 示例：
     * @code
     * int64_t value = 0;
     * if (const auto *in = gctx->input(2))
     *     in->read_int("value", &value);
     * @endcode
     */
    bool read_int(const char *key, int64_t *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_int(key, out);
    }

    /** @brief 严格读取布尔值；成功返回 true，缺失、类型不符或空 out 返回 false 且不改 out。
     * 示例：
     * @code
     * bool value = false;
     * if (const auto *in = gctx->input(2))
     *     in->read_bool("value", &value);
     * @endcode
     */
    bool read_bool(const char *key, bool *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_bool(key, out);
    }

    /** @brief 严格读取 JSON 文本；成功返回 true，缺失、类型不符或空 out 返回 false 且不改 out。
     * 示例：
     * @code
     * std::string value = "";
     * if (const auto *in = gctx->input(2))
     *     in->read_json("value", &value);
     * @endcode
     */
    bool read_json(const char *key, std::string *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_json(key, out);
    }

    /** @brief 读取字符串；缺失或类型不符时返回调用方指定的 fallback。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     auto value = in->get_string("value", "");
     *     (void)value;
     * }
     * @endcode
     */
    std::string get_string(const char *key, const std::string &fallback = {}) const
    {
        std::string value;
        return read_string(key, &value) ? value : fallback;
    }

    /** @brief 读取double 数值；缺失或类型不符时返回调用方指定的 fallback。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     auto value = in->get_number("value", 0.0);
     *     (void)value;
     * }
     * @endcode
     */
    double get_number(const char *key, double fallback = 0.0) const
    {
        double value = fallback;
        return read_number(key, &value) ? value : fallback;
    }

    /** @brief 读取int64 整数；缺失或类型不符时返回调用方指定的 fallback。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     auto value = in->get_int("value", 0);
     *     (void)value;
     * }
     * @endcode
     */
    int64_t get_int(const char *key, int64_t fallback = 0) const
    {
        int64_t value = fallback;
        return read_int(key, &value) ? value : fallback;
    }

    /** @brief 读取布尔值；缺失或类型不符时返回调用方指定的 fallback。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     auto value = in->get_bool("value", false);
     *     (void)value;
     * }
     * @endcode
     */
    bool get_bool(const char *key, bool fallback = false) const
    {
        bool value = fallback;
        return read_bool(key, &value) ? value : fallback;
    }

    /** @brief 读取JSON 文本；缺失或类型不符时返回调用方指定的 fallback。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     auto value = in->get_json("value", "");
     *     (void)value;
     * }
     * @endcode
     */
    std::string get_json(const char *key, const std::string &fallback = {}) const
    {
        std::string value;
        return read_json(key, &value) ? value : fallback;
    }

  private:
    const ChannelLogicSnapshot *snapshot_ = nullptr;
    friend struct GlobalContext;
};

struct GlobalContext
{
    /** @brief 返回业务画布宽度 640，不触发取帧。
     * 示例：
     * @code
     * int width = gctx->business_width();
     * @endcode
     */
    int business_width() const
    {
        return business_coordinates::WIDTH;
    }
    /** @brief 返回业务画布高度 640，不触发取帧。
     * 示例：
     * @code
     * int height = gctx->business_height();
     * @endcode
     */
    int business_height() const
    {
        return business_coordinates::HEIGHT;
    }
    /** @brief 返回业务画布尺寸 640×640，不触发取帧。
     * 示例：
     * @code
     * cv::Size size = gctx->business_size();
     * @endcode
     */
    cv::Size business_size() const
    {
        return business_coordinates::size();
    }
    /** @brief 返回业务画布中心 (320,320)，不触发取帧。
     * 示例：
     * @code
     * cv::Point center = gctx->business_center();
     * @endcode
     */
    cv::Point business_center() const
    {
        return business_coordinates::center();
    }

    /** 本全局 logic 实例配置，实例存活期间稳定。 */
    const GlobalLogicConfig *config = nullptr;

    /** 本次分析时间及间隔；依赖的文件暂停时，本全局逻辑等待且分析计时停止。
     * real_time_ms 是持续前进的系统单调毫秒，用于实际硬件时长；unix_ms 用于日期。 */
    uint64_t timestamp_ms = 0;
    uint64_t real_time_ms = 0;
    uint64_t unix_ms = 0;
    float dt_ms = 0.0f;

    /** 从 0 开始的 tick 序号及无数据更新时调度器采用的兜底周期。 */
    int64_t tick_id = 0;
    int effective_poll_interval_ms = 0;

    /** 采样本 tick 时系统当前不可变运行配置的 generation。 */
    uint64_t runtime_generation = 0;

    /** 本实例跨 tick 状态和已按 Schema 解析的全局 logic 参数。 */
    std::shared_ptr<void> *state = nullptr;

    /** @brief 首次按 args 构造本实例状态，后续复用；未绑定状态返回 nullptr。
     * @note 同一实例的帧/tick/Action 必须使用同一个 T；返回指针仅在回调内借用。
     * 示例：
     * @code
     * struct State
     * {
     *     int count = 0;
     * };
     * if (auto *s = gctx->get_state<State>())
     *     ++s->count;
     * @endcode
     */
    template <typename T, typename... Args> T *get_state(Args &&...args) const
    {
        if (!state)
            return nullptr;
        if (!*state)
            *state = std::make_shared<T>(std::forward<Args>(args)...);
        return static_cast<T *>(state->get());
    }

  private:
    const LogicParameterSet *logic_parameters = nullptr;

    /**
     * 框架构造的本 tick 固定输入批次；其中对象和查询返回的指针只在本次 logic 调用期间有效，
     * 不得缓存到 state 或异步线程。需要跨 tick 保存时复制所需标量/字符串。
     */
  private:
    friend struct VisionContextAccess;
    EventSink *event_sink = nullptr;
    const std::vector<ChannelLogicSnapshot> *channel_snapshots = nullptr; /* 应用全部通道 */
    const std::vector<int> *connected_channel_ids = nullptr;              /* Web 画布连入通道 */
    const std::vector<ChannelInput> *ready_inputs = nullptr; /* 框架已完成选择和有效性过滤的业务输入 */
    /* 框架维护的本 tick 自有叠加层更新；业务输入快照始终只读。 */
    mutable std::map<int, std::vector<DrawCommand>> image_draw_commands;

  public:
    /** @brief 检查解析后的参数表是否含 key（含 Schema 默认值）；空指针返回 false。
     * 示例：
     * @code
     * if (gctx->has_param("count"))
     * {
     *     auto n = gctx->param_int("count");
     *     (void)n;
     * }
     * @endcode
     */
    bool has_param(const char *key) const;
    /** @brief 按 Schema 参数名 key 读取数值（number 或 integer）；缺失、类型不符或超出 float 范围返回 0。
     * 示例：
     * @code
     * auto value = gctx->param_float("score");
     * @endcode
     */
    float param_float(const char *key) const;
    /** @brief 按 Schema 参数名 key 读取整数（integer）；缺失或类型不符返回 0。
     * 示例：
     * @code
     * auto value = gctx->param_int("count");
     * @endcode
     */
    int64_t param_int(const char *key) const;
    /** @brief 按 Schema 参数名 key 读取布尔值（boolean）；缺失或类型不符返回 false。
     * 示例：
     * @code
     * auto value = gctx->param_bool("enabled");
     * @endcode
     */
    bool param_bool(const char *key) const;
    /** @brief 按 Schema 参数名 key 读取字符串（string）；缺失或类型不符返回空字符串。
     * 示例：
     * @code
     * auto value = gctx->param_string("label");
     * @endcode
     */
    std::string param_string(const char *key) const;
    /** @brief 按 Schema 参数名 key 读取数组或对象的 JSON 文本；缺失或类型不符返回空字符串。
     * 示例：
     * @code
     * auto value = gctx->param_json("zones");
     * @endcode
     */
    std::string param_json(const char *key) const;

    /** @brief 替换当前全局实例拥有的通道叠加层；空 commands 清除，不影响其他实例。
     * @note 需要正式运行时绑定 config；通道 ID 必须来自实际配置。
     * 示例：
     * @code
     * gctx->set_channel_draw_commands(2, {}); // 清除此全局实例在通道 2 的绘图
     * @endcode
     */
    void set_channel_draw_commands(int channel_id, const std::vector<DrawCommand> &commands) const;

    /** @brief 返回已发布、在线且未过期的输入数量；有连线时选连入通道，否则选全部通道。
     * 示例：
     * @code
     * for (std::size_t i = 0; i < gctx->input_count(); ++i)
     * {
     *     const auto *in = gctx->input_at(i);
     *     (void)in;
     * }
     * @endcode
     */
    std::size_t input_count() const
    {
        return ready_inputs ? ready_inputs->size() : 0;
    }

    /** @brief 按批次下标借用有效输入；越界返回 nullptr，指针仅在本 tick 有效。
     * 示例：
     * @code
     * if (const auto *in = gctx->input_at(0))
     * {
     *     auto n = in->get_int("count");
     *     (void)n;
     * }
     * @endcode
     */
    const ChannelInput *input_at(std::size_t index) const
    {
        return ready_inputs && index < ready_inputs->size() ? &(*ready_inputs)[index] : nullptr;
    }

    /** @brief 按配置通道 ID 查找有效输入；未连入或无有效数据时返回 nullptr。
     * 示例：
     * @code
     * if (const auto *in = gctx->input(2))
     * {
     *     auto n = in->get_int("count");
     *     (void)n;
     * }
     * @endcode
     */
    const ChannelInput *input(int configured_id) const
    {
        if (!ready_inputs)
            return nullptr;
        for (const auto &item : *ready_inputs)
            if (item.channel_id() == configured_id)
                return &item;
        return nullptr;
    }

    /** @brief 借用本 tick 的原始通道快照；不自动排除离线/过期数据，找不到返回 nullptr。
     * 示例：
     * @code
     * if (const auto *s = gctx->channel(2))
     * {
     *     bool fresh = s->readable(500);
     *     (void)fresh;
     * }
     * @endcode
     */
    const ChannelLogicSnapshot *channel(int configured_id) const
    {
        if (!channel_snapshots)
            return nullptr;
        for (const auto &snapshot : *channel_snapshots)
            if (snapshot.channel_id == configured_id)
                return &snapshot;
        return nullptr;
    }

    /** @brief 深拷贝本 tick 固定版本的通道证据；后续新帧不会替换此版本。
     * @return 空 out、无对应通道或无图时 false；仅取证时使用，避免每 tick 拷贝。
     * 示例：
     * @code
     * ChannelFrameSnapshot s;
     * if (gctx->get_channel_frame_snapshot(2, &s))
     * {
     *     cv::Mat image = s.frame;
     * }
     * @endcode
     */
    bool get_channel_frame_snapshot(int configured_id, ChannelFrameSnapshot *out) const;
};

/** @brief 全局 tick 回调；只在回调期间借用 gctx 及输入。
 * 示例：
 * @code
 * GlobalLogicFunc callback = [](GlobalContext *ctx) {
 *     auto count = ctx->input_count();
 *     (void)count;
 * };
 * @endcode
 */
typedef void (*GlobalLogicFunc)(GlobalContext *gctx);
/** @brief 全局动作回调；action 仅在本次调用期间有效，先检查非空。
 * 示例：
 * @code
 * GlobalLogicActionFunc callback = [](GlobalContext *, const LogicAction *) {
 *     return LogicActionResult{true, "ok"};
 * };
 * @endcode
 */
typedef LogicActionResult (*GlobalLogicActionFunc)(GlobalContext *gctx, const LogicAction *action);

/** @brief 启动线程前注册帧/tick 回调；复制名称，同名覆盖；空名称或空回调忽略。
 * 示例：
 * @code
 * register_global_logic("count", [](GlobalContext *c) {
 *     (void)c;
 * });
 * @endcode
 */
void register_global_logic(const char *name, GlobalLogicFunc func);
/** @brief 为同名业务注册 Action 回调；启动前调用，同名覆盖，空名称或空回调忽略。
 * 示例：
 * @code
 * register_global_logic_action("count", [](GlobalContext *, const LogicAction *) {
 *     return LogicActionResult{true, "ok"};
 * });
 * @endcode
 */
void register_global_logic_action(const char *name, GlobalLogicActionFunc func);

struct GlobalLogicRegistrar
{
    /** @brief 构造时注册回调；通常由 REGISTER_GLOBAL_LOGIC 宏在文件作用域使用。
     * 示例：
     * @code
     * GlobalLogicRegistrar reg("count", [](GlobalContext *c) {
     *     (void)c;
     * });
     * @endcode
     */
    GlobalLogicRegistrar(const char *name, GlobalLogicFunc func)
    {
        register_global_logic(name, func);
    }
};

/** @brief 在 .cpp 文件作用域注册全局 tick 回调，函数名作为 logic 名称。
 * 独立测试中没有可调参数时，不必额外调用 set_logic_catalog()。
 * 示例：
 * @code
 * REGISTER_GLOBAL_LOGIC(total_people);
 * @endcode
 */
#define REGISTER_GLOBAL_LOGIC(func) static const GlobalLogicRegistrar _global_logic_reg_##func(#func, func)

struct GlobalLogicActionRegistrar
{
    /** @brief 构造时注册业务动作；通常使用对应 REGISTER_*_ACTION 宏。
     * 示例：
     * @code
     * GlobalLogicActionRegistrar reg("count", [](GlobalContext *, const LogicAction *) {
     *     return LogicActionResult{true, "ok"};
     * });
     * @endcode
     */
    GlobalLogicActionRegistrar(const char *name, GlobalLogicActionFunc func)
    {
        register_global_logic_action(name, func);
    }
};

/** @brief 在文件作用域给指定全局 logic 绑定动作回调。
 * 示例：
 * @code
 * REGISTER_GLOBAL_LOGIC_ACTION(total_people, reset_total);
 * @endcode
 */
#define REGISTER_GLOBAL_LOGIC_ACTION(logic_func, func)                                                                 \
    static const GlobalLogicActionRegistrar _global_logic_action_reg_##func(#logic_func, func)
