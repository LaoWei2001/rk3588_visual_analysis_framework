#pragma once

namespace npu
{
struct AutoCorePlacement
{
    int pool = 7;
    int key() const
    {
        return pool;
    }
};
template <typename Model> inline bool active_core_model(const Model &model)
{
    return model.enable && !model.model_path.empty() && !model.model_type.empty();
}

template <typename Channel> inline bool has_automatic_models(const Channel &channel)
{
    if (!channel.enable || !channel.infer_enable)
        return false;
    for (const auto &model : channel.models)
        if (active_core_model(model) && model.npu_core == -1)
            return true;
    return false;
}

// Build one plan from the whole effective configuration, not from an isolated
// channel. Pinned models have first choice; AUTO uses the remaining cores.
// If every core is pinned, sharing is unavoidable and the common gate still
// serializes overlapping contexts. This rule is independent of model family.
template <typename Channels> inline int explicitly_assigned_cores(const Channels &channels)
{
    int assigned = 0;
    for (const auto &channel : channels)
    {
        if (!channel.enable || !channel.infer_enable)
            continue;
        for (const auto &model : channel.models)
            if (active_core_model(model) && model.npu_core >= 0 && model.npu_core < 3)
                assigned |= 1 << model.npu_core;
    }
    return assigned;
}

template <typename Channels> inline int preferred_auto_core_pool(const Channels &channels)
{
    const int free = 7 & ~explicitly_assigned_cores(channels);
    return free ? free : 7;
}

// The scheduling unit is an independent analysis channel. Children processing
// the same frame cooperate within that channel's pool instead of competing for
// separate latency budgets. No model-family or filename heuristic is involved.
template <typename Channels> inline AutoCorePlacement channel_auto_placement(const Channels &channels, int channel_id)
{
    const int pool = preferred_auto_core_pool(channels);
    int cores[3], available = 0, groups = 0, ordinal = 0;
    for (int core = 0; core < 3; ++core)
        if (pool & (1 << core))
            cores[available++] = core;
    for (const auto &channel : channels)
    {
        if (!has_automatic_models(channel))
            continue;
        ++groups;
        if (channel.id < channel_id)
            ++ordinal;
    }
    // Isolate only when channels and available cores match. Partially isolated
    // pools (e.g. two channels each with a private core plus a shared third)
    // gave workers unequal queue depths and regressed published depth results
    // in board A/B tests. Keep balanced striping in that case, as for overload.
    if (groups <= 1 || groups != available || explicitly_assigned_cores(channels) == 7)
        return {pool};
    return {1 << cores[ordinal % groups]};
}

// Give AUTO a concrete affinity within its channel's planned pool. The default
// arguments retain legacy striping for direct callers and oversubscribed pools.
inline int assigned_core_mask(int configured_core, int channel, int worker, int model_index, int auto_pool = 7)
{
    if (configured_core >= 0 && configured_core < 3)
        return 1 << configured_core;
    auto_pool &= 7;
    if (!auto_pool)
        auto_pool = 7;
    int cores[3], count = 0;
    for (int core = 0; core < 3; ++core)
        if (auto_pool & (1 << core))
            cores[count++] = core;
    return 1 << cores[(channel % count + worker % count + model_index % count) % count];
}
} // namespace npu
