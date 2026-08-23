/**
 * @file global_logic.h
 * @brief 跨通道全局算法接口与轮询调度
 *
 * 全局算法每个 tick 通过 GlobalContext::inputs() 接收一批已经过框架筛选的 ChannelInput。
 * Web 画布有连线时使用连入通道，否则使用应用全部通道；离线、尚未发布或长期没有
 * 更新的通道不会进入业务输入。底层仍保留原始快照接口供版本对齐和媒体同步使用。
 *
 * 通道私有状态不是跨模块接口。通道 logic 必须在 logic.json 的 outputs 中声明业务
 * 变量，并通过 ctx->publish_*() 发布；全局算法只依赖这份公开数据契约。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/config.h"
#include "runtime/app_ctrl.h"
#include "event/event_report.h"
#include "logic_action.h"
#include "logic_parameters.h"

/**
 * @brief 本全局实例从上个 tick 到本 tick 观察到的通道版本变化。
 *
 * initial_snapshot 表示全局实例启动后首次看到该通道的已有状态。之后
 * revision_count 大于 1 表示轮询期间该通道发生过多次发布；全局算法得到的是最新
 * 状态，若业务要求逐事件不丢失，应使用事件队列而不是瞬时 outputs。
 */
struct ChannelUpdate
{
    int channel_id = -1;
    bool initial_snapshot = false;
    uint64_t previous_publication_seq = 0;
    uint64_t publication_seq = 0;
    uint64_t revision_count = 0;
    uint64_t missed_revisions = 0;
    uint64_t published_steady_ms = 0;
};

/**
 * @brief 全局 Logic 的业务输入视图。
 *
 * 只有已经发布、当前在线且仍然有效的通道才会由框架放入 inputs()。本类型刻意不暴露
 * 发布时间和数据年龄；普通跨通道业务只读取公开变量，输入有效性由调度器统一负责。
 */
class ChannelInput
{
  public:
    ChannelInput() = default;
    explicit ChannelInput(const ChannelLogicSnapshot *snapshot) : snapshot_(snapshot) {}

    int channel_id() const
    {
        return snapshot_ ? snapshot_->channel_id : -1;
    }

    int64_t frame_id() const
    {
        return snapshot_ ? snapshot_->logic_frame_id : 0;
    }

    int src_width() const
    {
        return snapshot_ ? snapshot_->src_width : 0;
    }

    int src_height() const
    {
        return snapshot_ ? snapshot_->src_height : 0;
    }

    bool infer_enabled() const
    {
        return snapshot_ && snapshot_->infer_enabled;
    }

    const std::string &logic_name() const
    {
        static const std::string empty;
        return snapshot_ ? snapshot_->logic_name : empty;
    }

    bool has(const char *key) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->has(key);
    }

    bool read_string(const char *key, std::string *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_string(key, out);
    }

    bool read_number(const char *key, double *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_number(key, out);
    }

    bool read_int(const char *key, int64_t *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_int(key, out);
    }

    bool read_bool(const char *key, bool *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_bool(key, out);
    }

    bool read_json(const char *key, std::string *out) const
    {
        return snapshot_ && snapshot_->outputs && snapshot_->outputs->try_get_json(key, out);
    }

    /* 与 GlobalContext::param_*() 一致的简洁取值形式。字段缺失或类型不匹配时返回
     * 调用方给出的默认值；需要区分“缺失”和合法零值时使用上面的 read_*()。 */
    std::string get_string(const char *key, const std::string &fallback = {}) const
    {
        std::string value;
        return read_string(key, &value) ? value : fallback;
    }

    double get_number(const char *key, double fallback = 0.0) const
    {
        double value = fallback;
        return read_number(key, &value) ? value : fallback;
    }

    int64_t get_int(const char *key, int64_t fallback = 0) const
    {
        int64_t value = fallback;
        return read_int(key, &value) ? value : fallback;
    }

    bool get_bool(const char *key, bool fallback = false) const
    {
        bool value = fallback;
        return read_bool(key, &value) ? value : fallback;
    }

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
    /** 本全局 logic 实例配置，实例存活期间稳定。 */
    const GlobalLogicConfig *config = nullptr;

    /** 本 tick 的单调时间、墙钟时间及距上个 tick 的真实间隔。字段名与 ChannelContext 一致。 */
    uint64_t timestamp_ms = 0;
    uint64_t unix_ms = 0;
    float dt_ms = 0.0f;

    /** 从 0 开始的 tick 序号及调度器实际采用的轮询周期。 */
    int64_t tick_id = 0;
    int effective_poll_interval_ms = 0;

    /** 采样本 tick 时系统当前不可变运行配置的 generation。 */
    uint64_t runtime_generation = 0;

    /** 本实例跨 tick 状态和已按 Schema 解析的全局 logic 参数。 */
    std::shared_ptr<void> *state = nullptr;
    const LogicParameterSet *logic_parameters = nullptr;

    /**
     * 框架构造的本 tick 固定输入批次；其中对象和查询返回的指针只在本次 logic 调用期间有效，
     * 不得缓存到 state 或异步线程。需要跨 tick 保存时复制所需标量/字符串。
     */
    const std::vector<ChannelLogicSnapshot> *channel_snapshots = nullptr; /* 应用全部通道 */
    const std::vector<int> *connected_channel_ids = nullptr;              /* Web 画布连入通道 */
    const std::vector<ChannelUpdate> *updated_channels = nullptr;
    const std::vector<ChannelInput> *ready_inputs = nullptr; /* 框架已完成选择和有效性过滤的业务输入 */

    bool has_param(const char *key) const;
    float param_float(const char *key) const;
    int64_t param_int(const char *key) const;
    bool param_bool(const char *key) const;
    std::string param_string(const char *key) const;
    std::string param_json(const char *key) const;

    std::size_t channel_count() const
    {
        return channel_snapshots ? channel_snapshots->size() : 0;
    }

    std::size_t connected_channel_count() const
    {
        return connected_channel_ids ? connected_channel_ids->size() : 0;
    }

    /**
     * 全局业务的默认输入入口：有画布连线时返回连入通道，否则返回应用全部通道；
     * 尚未发布、离线或长时间没有更新的通道已由框架排除。
     */
    const std::vector<ChannelInput> &inputs() const
    {
        static const std::vector<ChannelInput> empty;
        return ready_inputs ? *ready_inputs : empty;
    }

    const ChannelInput *input(int configured_id) const
    {
        if (!ready_inputs)
            return nullptr;
        for (const auto &item : *ready_inputs)
            if (item.channel_id() == configured_id)
                return &item;
        return nullptr;
    }

    /* 以下原始快照接口供需要版本、更新时间或媒体一致性的高级逻辑使用。 */
    const ChannelLogicSnapshot *channel_at(std::size_t index) const
    {
        return channel_snapshots && index < channel_snapshots->size() ? &(*channel_snapshots)[index] : nullptr;
    }

    const ChannelLogicSnapshot *channel(int configured_id) const
    {
        if (!channel_snapshots)
            return nullptr;
        for (const auto &snapshot : *channel_snapshots)
            if (snapshot.channel_id == configured_id)
                return &snapshot;
        return nullptr;
    }

    bool contains_channel(int configured_id) const
    {
        return channel(configured_id) != nullptr;
    }

    bool is_connected_channel(int configured_id) const
    {
        if (!connected_channel_ids)
            return false;
        for (int channel_id : *connected_channel_ids)
            if (channel_id == configured_id)
                return true;
        return false;
    }

    const ChannelLogicSnapshot *connected_channel_at(std::size_t index) const
    {
        return connected_channel_ids && index < connected_channel_ids->size()
                   ? channel((*connected_channel_ids)[index])
                   : nullptr;
    }

    bool has_updates() const
    {
        return updated_channels && !updated_channels->empty();
    }

    const ChannelUpdate *channel_update(int configured_id) const
    {
        if (!updated_channels)
            return nullptr;
        for (const auto &update : *updated_channels)
            if (update.channel_id == configured_id)
                return &update;
        return nullptr;
    }

    bool channel_updated(int configured_id) const
    {
        return channel_update(configured_id) != nullptr;
    }

    const ChannelUpdate *latest_update() const
    {
        const ChannelUpdate *latest = nullptr;
        if (!updated_channels)
            return nullptr;
        for (const auto &update : *updated_channels)
            if (!latest || update.published_steady_ms > latest->published_steady_ms)
                latest = &update;
        return latest;
    }

    /** 深拷贝本 tick 对应的媒体快照；通道已更新到下一版时返回 false，绝不混用版本。 */
    bool get_channel_frame_snapshot(int configured_id, ChannelFrameSnapshot *out) const
    {
        const ChannelLogicSnapshot *expected = channel(configured_id);
        return out && expected && app_ctrl_get_channel_frame_snapshot(configured_id, out) != 0 &&
               out->logic.publication_seq == expected->publication_seq;
    }

    template <typename Func> void for_each_channel(Func &&fn) const
    {
        if (!channel_snapshots)
            return;
        for (std::size_t i = 0; i < channel_snapshots->size(); ++i)
            fn((*channel_snapshots)[i], static_cast<int>(i));
    }

    template <typename Func> void for_each_connected_channel(Func &&fn) const
    {
        if (!connected_channel_ids)
            return;
        for (std::size_t i = 0; i < connected_channel_ids->size(); ++i)
        {
            const ChannelLogicSnapshot *snapshot = channel((*connected_channel_ids)[i]);
            if (snapshot)
                fn(*snapshot, static_cast<int>(i));
        }
    }

    template <typename Func> void for_each_updated_channel(Func &&fn) const
    {
        if (!updated_channels)
            return;
        for (std::size_t i = 0; i < updated_channels->size(); ++i)
        {
            const ChannelUpdate &update = (*updated_channels)[i];
            const ChannelLogicSnapshot *snapshot = channel(update.channel_id);
            if (snapshot)
                fn(*snapshot, update, static_cast<int>(i));
        }
    }
};

typedef void (*GlobalLogicFunc)(GlobalContext *gctx);
typedef LogicActionResult (*GlobalLogicActionFunc)(GlobalContext *gctx, const LogicAction *action);

#define MAX_GLOBAL_LOGIC_FUNCS 64

GlobalLogicFunc global_logic_get(const char *name);
GlobalLogicActionFunc global_logic_action_get(const char *name);
std::vector<std::string> global_logic_names();
void register_global_logic(const char *name, GlobalLogicFunc func);
void register_global_logic_action(const char *name, GlobalLogicActionFunc func);

struct GlobalLogicRegistrar
{
    GlobalLogicRegistrar(const char *name, GlobalLogicFunc func)
    {
        register_global_logic(name, func);
    }
};

#define REGISTER_GLOBAL_LOGIC(func) static const GlobalLogicRegistrar _global_logic_reg_##func(#func, func)

struct GlobalLogicActionRegistrar
{
    GlobalLogicActionRegistrar(const char *name, GlobalLogicActionFunc func)
    {
        register_global_logic_action(name, func);
    }
};

#define REGISTER_GLOBAL_LOGIC_ACTION(logic_func, func)                                                           \
    static const GlobalLogicActionRegistrar _global_logic_action_reg_##func(#logic_func, func)

int global_logic_start_all(const std::vector<GlobalLogicConfig> &cfgs);
int global_logic_reload_all(const std::vector<GlobalLogicConfig> &cfgs);
void global_logic_stop_all(void);
int global_logic_get_instance_count(void);
void *global_logic_thread_func(void *arg);
