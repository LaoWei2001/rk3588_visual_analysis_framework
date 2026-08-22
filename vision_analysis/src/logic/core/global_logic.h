/**
 * @file global_logic.h
 * @brief 跨通道全局算法接口与轮询调度
 *
 * 全局算法每个 tick 接收应用全部通道的一批固定 ChannelLogicSnapshot。Web 画布连线
 * 只是其中一个可选子集；业务既可以遍历连入通道，也可以按 ID 读取或遍历全部通道。
 * 该批次只复制业务变量和元信息，不复制图像，同一函数调用内重复查询不会跨版本。
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
