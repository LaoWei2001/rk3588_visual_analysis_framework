/* ============================================================================
 * @file logic_path_sop.cpp
 * logic_path_sop —— 目标 SOP 路径合规检测 (单目标·按类别·允许任意有向图, 含环)
 *
 * 目标沿 flow 配置图的合法边推进。步骤、边、入口、出口及阈值均存放在
 * logic_parameters.flow 中，网页子画布和 C++ 运行时使用同一份结构化数据。
 * 图允许: 同源多出(分岔) / 多源汇合 / 反向边 / 环 / 多入口 / 多出口。
 * 例如 a→b→c 与 a→d→c 共存; b→a 形成短环; a→b, b→a, a 走一次返回再走 b 也合法。
 *
 * 起点: 被标记为「🚩 起点」的 step(entries = 这些 step 的索引), 即"一条路线的第一步"。
 * 允许多起点(多条路线)+ 多个起点共用同一区域。
 * 路线确定靠"并行候选 + 逐步收敛": 目标进入某区域 → 当前候选里所有匹配该区域的 step 都作为候选
 * (典型: 多个起点同区域 → 都候选); 目标走到下一个区域 → 只有匹配的分支存活, 其余被悄悄放弃
 * (不计 dwell 违规)。如此一步步收敛到唯一路线; 一旦只剩一条, 之后严格只走那条(锁定)。
 *
 * 并行候选漫游: 目标进入某 zone 时, 当前合法后继里所有匹配该 zone 的 step 都作为"候选路线"
 * 同时纳入 cur_steps; 之后目标走出区分动作, 只有匹配的分支存活, 其余分支被悄悄放弃(放弃的
 * 分支不计 dwell 违规)。这样"同 zone 多起点/多分支"在进入瞬间无法区分, 但会在下一步自然收敛。
 *
 * 五类报警(屏幕红字; 连了"上报配置"节点则同时上报):
 *   ① 顺序错误: 目标进入了"设计内区域", 但不是当前 active set 的合法后继 → 即时报警。
 *   ② 漏掉区域: 工序结束时, 仍有未访问过的 step → 结算报警(列出未走的 zone)。
 *   ③ 停留不足: 离开某步时, 该步停留 < 配置的最小停留 → 报警(每步每轮一次)。
 *   ④ 停留超时: 在某步停留 > 配置的最大停留(0=不限) → 即时报警(每步每轮一次)。
 *   ⑤ 总耗时上下限: settle 时, 总耗时 > 上限="总耗时超时" / < 下限="总耗时不足"。
 * 上报内容: 两张图 —— 一张叠加了检测框/区域/状态文字(同视频窗口), 一张原始视频帧。
 * 正常结果: report_normal=true 时，一轮正式结算、到达合法出口且没有任何违规，
 *           额外上报一次 sop_normal。
 *
 * 何时算"工序结束"(决定漏检何时结算), 由 flow.end_mode 选:
 *   - "leave"  (离场超时): 目标持续检测不到超过 reset_sec 秒 → 工序结束。
 *                          走到 exit step / 按序走完不立即结束 —— 只标记 completed, 仍等离场。
 *                          这样环图/回边场景下, 目标走完 exit 还能继续走环, 直到真正离场才 settle。
 *   - "endzone"(终点区域): 目标在 end_zone 连续停留达到 end_dwell_sec → 工序结束;
 *                          目标中途离场超过 reset_sec 秒 → 兜底也判结束;
 *                          按序走完全部步骤【不立即结束】 —— 需等"进入终点"或"离场超时"才 settle,
 *                          这样最后一步的 dwell_min 仍可正常累计/检查, 终点区域也才真正生效。
 *   - "trigger"(外部触发): 收到 sop_end_trigger 外部信号 → 工序结束(与开始的 sop_trigger 对称);
 *                          目标离场超过 reset_sec 秒 → 兜底也判结束(防卡死);
 *                          外部信号可由按钮、PLC、扫码枪等通过 API 发送, 触发方式不局限于按钮。
 *   settle 完成当帧, 主函数末尾会调用 reset_state 把所有 runtime + 显示 latch 清零 →
 *   下一帧屏幕回到初始空白(右上 6 行全绿, 步骤列表全灰, 总耗时"待开始")。
 *   报警当帧已通过 alarms 队列 + 叠加图上报, 历史可在上报记录里查; 屏幕不保留上一轮痕迹。
 *   无论何路径 settle, 最后所在步的 dwell_min 都会在 settle 内被统一判一次。
 *
 * flow.steps[].zoneName 必须与本通道 ROI 名完全一致。参数热更新策略为 reset_state，
 * 因此配置图变化时不会把旧图的运行状态带到新图。
 * ==========================================================================*/
#include "logic/core/logic_common.h"
#include "third_party/json/cJSON.h"

#include <cstdio>

struct PathSopStepConfig
{
    std::string zone;
    float enter_sec = 0.5f;
    float dwell_min_sec = 0.0f;
    float dwell_max_sec = 0.0f;
};

struct PathSopEdgeLimit
{
    int src = -1;
    int dst = -1;
    int min_count = 0;
    int max_count = 0;
};

struct PathSopFlowConfig
{
    std::string target_label;
    float reset_sec = 5.0f;
    std::string end_mode = "leave";
    std::string end_zone;
    float end_dwell_sec = 0.0f;
    float total_min_sec = 0.0f;
    float total_max_sec = 0.0f;
    std::string trigger_mode = "auto";
    bool trigger_mandatory = false;
    bool report_normal = false;
    std::vector<PathSopStepConfig> steps;
    std::vector<std::pair<int, int>> edges;
    std::vector<PathSopEdgeLimit> edge_limits;
    std::vector<int> entries;
    std::vector<int> exits;
};

static float path_json_number(const cJSON *object, const char *key, float fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : fallback;
}

static std::string path_json_string(const cJSON *object, const char *key, const std::string &fallback = {})
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

static bool path_json_bool(const cJSON *object, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static std::vector<int> path_json_indices(const cJSON *root, const char *key, int step_count)
{
    std::vector<int> result;
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsArray(array))
        return result;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, array)
    {
        if (!cJSON_IsNumber(item))
            continue;
        const int value = item->valueint;
        if (value >= 0 && value < step_count && std::find(result.begin(), result.end(), value) == result.end())
            result.push_back(value);
    }
    return result;
}

static PathSopFlowConfig path_parse_flow(const std::string &json)
{
    PathSopFlowConfig flow;
    cJSON *root = cJSON_Parse(json.c_str());
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return flow;
    }

    flow.target_label = path_json_string(root, "target_label");
    flow.reset_sec = std::max(0.0f, path_json_number(root, "reset_sec", 5.0f));
    flow.end_mode = path_json_string(root, "end_mode", "leave");
    if (flow.end_mode != "leave" && flow.end_mode != "endzone" && flow.end_mode != "trigger")
        flow.end_mode = "leave";
    flow.end_zone = path_json_string(root, "end_zone");
    flow.end_dwell_sec = std::max(0.0f, path_json_number(root, "end_dwell_sec", 0.0f));
    flow.total_min_sec = std::max(0.0f, path_json_number(root, "total_min_sec", 0.0f));
    flow.total_max_sec = std::max(0.0f, path_json_number(root, "total_max_sec", 0.0f));
    flow.trigger_mode = path_json_string(root, "trigger_mode", "auto");
    if (flow.trigger_mode != "auto" && flow.trigger_mode != "external")
        flow.trigger_mode = "auto";
    flow.trigger_mandatory = path_json_bool(root, "trigger_mandatory", false);
    flow.report_normal = path_json_bool(root, "report_normal", false);

    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    const cJSON *step = nullptr;
    cJSON_ArrayForEach(step, steps)
    {
        if (!cJSON_IsObject(step))
            continue;
        PathSopStepConfig parsed;
        parsed.zone = path_json_string(step, "zoneName");
        parsed.enter_sec = std::max(0.0f, path_json_number(step, "enter_sec", 0.5f));
        parsed.dwell_min_sec = std::max(0.0f, path_json_number(step, "dwell_min_sec", 0.0f));
        parsed.dwell_max_sec = std::max(0.0f, path_json_number(step, "dwell_max_sec", 0.0f));
        flow.steps.push_back(parsed);
    }

    const int step_count = static_cast<int>(flow.steps.size());
    const cJSON *edges = cJSON_GetObjectItemCaseSensitive(root, "edges");
    const cJSON *edge = nullptr;
    cJSON_ArrayForEach(edge, edges)
    {
        if (!cJSON_IsArray(edge) || cJSON_GetArraySize(edge) != 2)
            continue;
        const cJSON *src = cJSON_GetArrayItem(edge, 0);
        const cJSON *dst = cJSON_GetArrayItem(edge, 1);
        if (!cJSON_IsNumber(src) || !cJSON_IsNumber(dst))
            continue;
        const int a = src->valueint, b = dst->valueint;
        if (a >= 0 && a < step_count && b >= 0 && b < step_count &&
            std::find(flow.edges.begin(), flow.edges.end(), std::make_pair(a, b)) == flow.edges.end())
            flow.edges.push_back({a, b});
    }

    const cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "edge_limits");
    const cJSON *limit = nullptr;
    cJSON_ArrayForEach(limit, limits)
    {
        if (!cJSON_IsObject(limit))
            continue;
        PathSopEdgeLimit parsed;
        parsed.src = static_cast<int>(path_json_number(limit, "src", -1));
        parsed.dst = static_cast<int>(path_json_number(limit, "dst", -1));
        parsed.min_count = std::max(0, static_cast<int>(path_json_number(limit, "min", 0)));
        parsed.max_count = std::max(0, static_cast<int>(path_json_number(limit, "max", 0)));
        if (parsed.src >= 0 && parsed.src < step_count && parsed.dst >= 0 && parsed.dst < step_count &&
            (parsed.min_count > 0 || parsed.max_count > 0))
            flow.edge_limits.push_back(parsed);
    }

    flow.entries = path_json_indices(root, "entries", step_count);
    flow.exits = path_json_indices(root, "exits", step_count);
    cJSON_Delete(root);
    return flow;
}

static cJSON *path_string_array(const std::vector<std::string> &values)
{
    cJSON *array = cJSON_CreateArray();
    for (const auto &value : values)
        cJSON_AddItemToArray(array, cJSON_CreateString(value.c_str()));
    return array;
}

static std::string path_json_text(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    std::string result = text ? text : "{}";
    if (text)
        cJSON_free(text);
    cJSON_Delete(root);
    return result;
}

struct PathSopState
{
    bool flow_loaded = false;
    PathSopFlowConfig flow;
    int expect = 0;             /* 已访问 step 数, 用于进度显示 N/M */
    std::vector<int> cur_steps; /* 当前所在 step 集合(空 = 入口前/已结束)。
                                 * 并行漫游: 进入同 zone 时, 所有当前合法后继中匹配 zone 的 step 都纳入 cur_steps,
                                 * 各自独立累计 dwell, 任一是 exit 节点 → completed。这样 DAG 上"同 zone 兄弟分支"
                                 * (例如 4→{5,6} 且 5/6 同区域)不再退化为只走第一条 → 给用户最大路径规划自由度。 */
    std::string cur_zone;       /* 最近确认所在区域名("" = 不在任何区域) */
    std::string cand_zone;      /* 候选区域(进入防抖) */
    uint64_t cand_since_ms = 0;
    std::vector<bool> visited_steps; /* 已访问 step 集合(DAG 漏检判定: 是否存在 entry→exit 完整路径) */
    std::vector<std::string> zone_history; /* 按实际确认顺序记录区域，允许重复，用于 Dify 二次核验 */
    std::vector<uint64_t> dwell_ms;
    std::vector<uint64_t> dwell_out_since; /* 每步: 目标离开该步区域的时刻(0=当前在内)。用于"重进则重新计时" */
    bool order_error = false;        /* 顺序错误(latch) */
    std::string err_detail;          /* 顺序错误详情: 进了X 应当进入Y */
    bool ended = false;              /* 本轮工序已结束(显示结算) */
    std::vector<std::string> missed; /* 结束时算出的漏掉区域 */
    bool completed = false;          /* 严格按序走完全部步骤 */
    uint64_t last_seen_ms = 0;
    std::vector<bool> short_done; /* 该步"停留不足"已报(本轮去重) */
    std::vector<bool> long_done;  /* 该步"停留超时"已报(本轮去重) */
    bool dwell_short = false;     /* 本轮出现过"停留不足"(显示用 latch) */
    bool dwell_over = false;      /* 本轮出现过"停留超时"(显示用 latch) */
    std::string dwell_detail;     /* 最近一次停留违规详情(显示用) */
    /* 总耗时(本轮): 进入第 1 步时启动, settle 时锁定 */
    uint64_t round_start_ms = 0; /* 0 = 本轮尚未开始计时 */
    uint64_t round_total_ms = 0; /* settle 时锁定的总耗时(过程中实时算, 见 grpah 末尾) */
    bool total_short = false;    /* settle 后: 总耗时不足(< path_total_min_sec) */
    bool total_over = false;     /* settle 后: 总耗时超时(> path_total_max_sec) */
    std::string total_detail;    /* 总耗时违规详情(显示用) */
    /* 循环边次数计数(key = (src*nseq + dst) 编码):
     * 参与有向环/自环的边，以及显式配置次数范围的边，每穿过一次 +1。 */
    std::vector<int> edge_count_dense; /* 密集存储 size=nseq*nseq; nseq 在每帧重算时检查 resize */
    bool loop_violation = false;       /* settle 后: 某条带 limit 的边循环次数不在范围内 */
    std::string loop_detail;           /* 循环次数违规详情(显示用) */
    bool triggered = false;            /* 外部触发标志: 收到sop_trigger后置true, settle时清回false */
    bool triggered_end = false; /* 外部结束触发标志: 收到sop_end_trigger后置true, settle时清回false */
    bool untriggered_entry = false; /* 外部触发必须模式下, 未触发却进区域(latch) */
    bool end_zone_tracking = false; /* 是否正在连续跟踪终点区域停留 */
    std::string tracked_end_zone;   /* 热更新终点区域时用于识别并重启计时 */
    uint64_t end_zone_since_ms = 0; /* 本次连续进入终点区域的起始时刻 */
};

static ChannelActionResult logic_path_sop_action(ChannelContext *ctx, const ChannelAction *action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state || !action)
    {
        result.message = "ctx or action is null";
        return result;
    }

    if (action->name == "start_new_run" || action->name == "reset")
    {
        *ctx->state = std::make_shared<PathSopState>();
        result.handled = true;
        result.message = "SOP state reset; waiting for next valid entry";
        return result;
    }

    if (action->name == "sop_trigger")
    {
        if (!*ctx->state)
            *ctx->state = std::make_shared<PathSopState>();
        auto &s = *std::static_pointer_cast<PathSopState>(*ctx->state);
        s.triggered = true;
        result.handled = true;
        result.message = "external trigger received";
        return result;
    }

    if (action->name == "sop_end_trigger")
    {
        if (!*ctx->state)
            *ctx->state = std::make_shared<PathSopState>();
        auto &s = *std::static_pointer_cast<PathSopState>(*ctx->state);
        s.triggered_end = true;
        result.handled = true;
        result.message = "external end trigger received";
        return result;
    }

    result.message = std::string("unsupported action: ") + action->name;
    return result;
}

static void logic_path_sop(ChannelContext *ctx)
{
    if (!ctx)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<PathSopState>();
    auto &s = *std::static_pointer_cast<PathSopState>(*ctx->state);

    /* ---- 参数：仅在状态创建时解析；flow 变更会由 x-hot-reload=reset_state 重建状态 ---- */
    if (!s.flow_loaded)
    {
        s.flow = path_parse_flow(ctx->param_json("flow"));
        s.flow_loaded = true;
    }
    const PathSopFlowConfig &flow = s.flow;
    std::vector<std::string> seq;
    seq.reserve(flow.steps.size());
    for (const auto &step : flow.steps)
        seq.push_back(step.zone);
    const std::string &targetLab = flow.target_label;
    const float endSec = flow.reset_sec;
    const uint64_t end_ms = (uint64_t)std::max(0, (int)(endSec * 1000.0f + 0.5f));
    const std::string &endMode = flow.end_mode;
    const std::string &endZone = flow.end_zone;
    const float endDwellSec = flow.end_dwell_sec;
    const uint64_t end_dwell_ms = (uint64_t)std::max(0, (int)(endDwellSec * 1000.0f + 0.5f));
    const bool useEndZone = (endMode == "endzone") && !endZone.empty();
    const bool useTriggerEnd = (endMode == "trigger");
    const int nseq = (int)seq.size();

    bool has_empty_zone = false;
    for (const auto &zone : seq)
        has_empty_zone = has_empty_zone || zone.empty();
    if (nseq < 1 || targetLab.empty() || has_empty_zone || flow.entries.empty() || flow.exits.empty() ||
        (nseq > 1 && flow.edges.empty()) || (endMode == "endzone" && endZone.empty()))
    {
        draw_text(ctx, "path_sop: flow 配置不完整（目标、步骤、边、入口、出口均需显式设置）", cv::Point(20, 30),
                  cv::Scalar(0, 165, 255), 0.55, 2);
        return;
    }
    if ((int)s.dwell_ms.size() != nseq)
        s.dwell_ms.assign(nseq, 0);
    if ((int)s.dwell_out_since.size() != nseq)
        s.dwell_out_since.assign(nseq, 0);

    /* ---- 每步进入确认/最小停留 ---- */
    std::vector<uint64_t> enter_ms(nseq);
    std::vector<float> dwell_min(nseq);
    for (int i = 0; i < nseq; ++i)
    {
        const float e = flow.steps[i].enter_sec;
        const float d = flow.steps[i].dwell_min_sec;
        enter_ms[i] = (uint64_t)std::max(0, (int)(e * 1000.0f + 0.5f));
        dwell_min[i] = std::max(0.0f, d);
    }

    /* ---- 每步最大停留(0 = 不限, 用户可忽略) ---- */
    std::vector<float> dwell_max(nseq);
    for (int i = 0; i < nseq; ++i)
        dwell_max[i] = flow.steps[i].dwell_max_sec;
    if ((int)s.short_done.size() != nseq)
        s.short_done.assign(nseq, false);
    if ((int)s.long_done.size() != nseq)
        s.long_done.assign(nseq, false);

    /* ---- 总耗时上下限(0 = 不限) ---- */
    const float totalMin = flow.total_min_sec;
    const float totalMax = flow.total_max_sec;

    /* ---- 显式边（允许任意图含环） ---- */
    std::vector<std::vector<int>> succ(nseq); /* succ[i] = i 的所有后继 step 索引 */
    std::vector<bool> self_loop(
        nseq, false); /* 该 step 是否画了自环(a==a): 语义=可"重进同区域", 用于把该区域也高亮成"可走" */
    for (const auto &edge : flow.edges)
    {
        const int a = edge.first, b = edge.second;
        if (a == b)
        {
            self_loop[a] = true;
            continue;
        }
        succ[a].push_back(b);
    }
    const std::vector<int> &entries = flow.entries;
    const std::vector<int> &exits = flow.exits;

    /* ---- 边循环次数约束解析 ----
     * 密集存储: limit_min[src*nseq+dst] / limit_max[src*nseq+dst]; 0 = 无约束。 */
    std::vector<int> limit_min(nseq * nseq, 0);
    std::vector<int> limit_max(nseq * nseq, 0);
    std::vector<std::pair<int, int>> limited_edges; /* 列出有约束的边, settle 时遍历判定 */
    for (const auto &limit : flow.edge_limits)
    {
        const int idx = limit.src * nseq + limit.dst;
        limit_min[idx] = limit.min_count;
        limit_max[idx] = limit.max_count;
        limited_edges.push_back({limit.src, limit.dst});
    }

    /* ---- 业务 JSON 需要完整呈现的循环边 ----
     * 1) 图中真正参与有向环的普通边；2) 自环；3) 显式配置了次数范围的边。
     * 第 3 类即使配置文件被手工改成了非环边，也保持旧行为并继续计数/判定。
     * 这样 min=max=0 的“任意次数”环边也会被统计并进入正常结果 JSON。 */
    std::vector<bool> tracked_loop_edge(nseq * nseq, false);
    for (const auto &edge : limited_edges)
        tracked_loop_edge[edge.first * nseq + edge.second] = true;
    for (int i = 0; i < nseq; ++i)
        if (self_loop[i])
            tracked_loop_edge[i * nseq + i] = true;
    for (int src = 0; src < nseq; ++src)
    {
        for (int dst : succ[src])
        {
            /* src→dst 属于环，当且仅当还能从 dst 沿有向边回到 src。 */
            std::vector<char> seen(nseq, 0);
            std::vector<int> stack(1, dst);
            bool returns_to_src = false;
            while (!stack.empty() && !returns_to_src)
            {
                const int node = stack.back();
                stack.pop_back();
                if (node == src)
                {
                    returns_to_src = true;
                    break;
                }
                if (node < 0 || node >= nseq || seen[node])
                    continue;
                seen[node] = 1;
                for (int next : succ[node])
                    if (!seen[next])
                        stack.push_back(next);
            }
            if (returns_to_src)
                tracked_loop_edge[src * nseq + dst] = true;
        }
    }
    std::vector<std::pair<int, int>> loop_edges;
    for (int src = 0; src < nseq; ++src)
        for (int dst = 0; dst < nseq; ++dst)
            if (tracked_loop_edge[src * nseq + dst])
                loop_edges.push_back({src, dst});

    /* 状态机用的 step 集合 + 边计数: 同步初始化 */
    if ((int)s.visited_steps.size() != nseq)
        s.visited_steps.assign(nseq, false);
    if ((int)s.edge_count_dense.size() != nseq * nseq)
        s.edge_count_dense.assign(nseq * nseq, 0);

    /* 当前合法下一步 step 索引集合: 入口前=entries; 否则=cur_steps 中所有 step 的 succ 并集(去重)。
     * 并行漫游: 用户在同一分岔点画多条"同 zone 兄弟分支"时, 这一函数会把所有分支的后继全列出来。 */
    auto current_next = [&]() -> std::vector<int> {
        if (s.cur_steps.empty())
            return entries;
        std::vector<int> out;
        std::vector<char> seen(nseq, 0);
        for (int u : s.cur_steps)
        {
            if (u < 0 || u >= nseq)
                continue;
            for (int v : succ[u])
                if (!seen[v])
                {
                    seen[v] = 1;
                    out.push_back(v);
                }
        }
        return out;
    };

    /* 在候选 step 集合里找 zone 名匹配的: 返回【全部】命中(支持"同 zone 多 step 兄弟分支") */
    auto match_steps = [&](const std::vector<int> &cand, const std::string &zone) -> std::vector<int> {
        std::vector<int> hits;
        for (int i : cand)
            if (i >= 0 && i < nseq && seq[i] == zone)
                hits.push_back(i);
        return hits;
    };

    /* 一个 zone 是否出现在 sequence 中(任意 step) — 用来区分"顺序错误"vs"路过无关区域" */
    auto is_designed_zone = [&](const std::string &zone) -> bool {
        for (const auto &z : seq)
            if (z == zone)
                return true;
        return false;
    };

    /* 完整重置 PathSopState 所有 runtime + 显示用 latch 字段, 回到"初始空白"状态。
     * 主函数末尾在 settle 完成的那一帧调用，让下一帧屏幕完全归零。 */
    auto reset_state = [&]() {
        s.expect = 0;
        s.cur_steps.clear();
        s.cur_zone.clear();
        s.cand_zone.clear();
        s.cand_since_ms = 0;
        s.last_seen_ms = 0;
        s.zone_history.clear();
        std::fill(s.visited_steps.begin(), s.visited_steps.end(), false);
        s.order_error = false;
        s.err_detail.clear();
        s.ended = false;
        s.missed.clear();
        s.completed = false;
        std::fill(s.dwell_ms.begin(), s.dwell_ms.end(), 0);
        std::fill(s.dwell_out_since.begin(), s.dwell_out_since.end(), 0);
        std::fill(s.short_done.begin(), s.short_done.end(), false);
        std::fill(s.long_done.begin(), s.long_done.end(), false);
        s.dwell_short = false;
        s.dwell_over = false;
        s.dwell_detail.clear();
        s.round_start_ms = 0;
        s.round_total_ms = 0;
        s.total_short = false;
        s.total_over = false;
        s.total_detail.clear();
        std::fill(s.edge_count_dense.begin(), s.edge_count_dense.end(), 0);
        s.loop_violation = false;
        s.loop_detail.clear();
        s.triggered = false;
        s.triggered_end = false;
        s.untriggered_entry = false;
        s.end_zone_tracking = false;
        s.tracked_end_zone.clear();
        s.end_zone_since_ms = 0;
    };

    /* ---- 目标 + 所在区域 ---- */
    AlgoResult *target = nullptr;
    if (ctx->results)
    {
        float best = -1.0f;
        for (auto &r : *ctx->results)
            if (r.label == targetLab && r.score > best)
            {
                best = r.score;
                target = &r;
            }
    }
    cv::Point center(-1, -1);
    if (target)
        center = target->box_center();

    std::string inZone;
    if (target && ctx->rois)
        for (const auto &z : *ctx->rois)
            if (z.polygon.size() >= 3 && cv::pointPolygonTest(z.polygon, center, false) >= 0.0)
            {
                inZone = z.name;
                break;
            }

    auto inVisited = [&](const std::string &nm) {
        for (int i = 0; i < nseq; ++i)
            if (s.visited_steps[i] && seq[i] == nm)
                return true;
        return false;
    };

    /* SOP事件稀疏, 不做业务层时间冷却:
     * 顺序错误本轮 latch 一次、漏检结束算一次、每步停留违规各一次; 开新一轮自然解锁、可再报。
     * 本帧告警先收集, 等叠加层都画好后, 在函数末尾统一渲染"叠加图 + 原始帧"两张上报。 */
    std::vector<const char *> alarms;
    auto raise = [&](const char *atype) { alarms.push_back(atype); };
    /* 离开某步时判其"最小停留": 不足则报一次(本轮去重) */
    auto judgeShort = [&](int step) {
        if (step < 0 || step >= nseq || s.short_done[step])
            return;
        if (dwell_min[step] <= 0.0f)
            return;
        const uint64_t need = (uint64_t)(dwell_min[step] * 1000.0f + 0.5f);
        if (s.dwell_ms[step] < need)
        {
            s.short_done[step] = true;
            s.dwell_short = true;
            char d[160];
            snprintf(d, sizeof(d), "步%d[%s] 停留%.1fs < %.1fs", step + 1, seq[step].c_str(),
                     s.dwell_ms[step] / 1000.0f, dwell_min[step]);
            s.dwell_detail = d;
            raise("sop_dwell_short");
        }
    };
    auto settle = [&]() { /* 工序结束 → 结算漏检 + 兜底判所有 active step 的 dwell_min + 总耗时判定 */
                          /* 集中处理: 无论从哪条路径结束(离场/进终点/走完), 所有"最后所在 active step"的 dwell_min
                           * 都被判一次 */
                          for (int u : s.cur_steps)
                              judgeShort(u);
                          s.missed.clear();
                          if (s.round_start_ms != 0)
                          {
                              /* 漏检判定:
                               *   exits 非空 → "visited 子图上, 从某个 visited 的 entry 能否走到某个 visited 的 exit"
                               *               有 = 合规(可以画 1→4 短路径而不必走 1→2→3→4);
                               *               无 = 漏检(列出未访问 step 的 zone 名).
                               *   exits 空(全环图无标注) → 退化为"全 visited 才合规"。 */
                              bool compliant = false;
                              if (!exits.empty())
                              {
                                  std::vector<bool> reachable(nseq, false);
                                  std::vector<int> stk;
                                  for (int e : entries)
                                      if (s.visited_steps[e])
                                          stk.push_back(e);
                                  while (!stk.empty())
                                  {
                                      const int u = stk.back();
                                      stk.pop_back();
                                      if (reachable[u] || !s.visited_steps[u])
                                          continue;
                                      reachable[u] = true;
                                      for (int v : succ[u])
                                          if (!reachable[v] && s.visited_steps[v])
                                              stk.push_back(v);
                                  }
                                  for (int x : exits)
                                      if (reachable[x])
                                      {
                                          compliant = true;
                                          break;
                                      }
                              }
                              else
                              {
                                  compliant = (s.expect >= nseq);
                              }
                              if (!compliant)
                              {
                                  std::vector<std::string> miss_zones;
                                  for (int i = 0; i < nseq; ++i)
                                  {
                                      if (s.visited_steps[i])
                                          continue;
                                      if (std::find(miss_zones.begin(), miss_zones.end(), seq[i]) == miss_zones.end())
                                          miss_zones.push_back(seq[i]);
                                  }
                                  s.missed = miss_zones;
                              }
                              if (!s.missed.empty() && !s.order_error)
                                  raise("sop_missed"); /* 漏检: 上报一次(顺序错误已解释, 不重复) */
                              /* 边循环次数判定: 遍历所有有 limit 的边, 检查实际 count 是否在 [min, max] 范围内 */
                              for (const auto &ed : limited_edges)
                              {
                                  const int eidx = ed.first * nseq + ed.second;
                                  const int cnt = s.edge_count_dense[eidx];
                                  const int mn = limit_min[eidx], mx = limit_max[eidx];
                                  bool bad = false;
                                  if (mn > 0 && cnt < mn)
                                      bad = true;
                                  if (mx > 0 && cnt > mx)
                                      bad = true;
                                  if (bad)
                                  {
                                      s.loop_violation = true;
                                      char d[160];
                                      snprintf(d, sizeof(d), "%s[%s]→[%s] 走%d次 (限 %d~%d)",
                                               s.loop_detail.empty() ? "" : "; ", seq[ed.first].c_str(),
                                               seq[ed.second].c_str(), cnt, mn, mx);
                                      s.loop_detail += d;
                                      raise("sop_loop_violation");
                                  }
                              }
                              /* 总耗时结算: 仅当本轮启动过计时(round_start_ms 非 0)且配了上下限时才判 */
                              if (s.round_start_ms != 0)
                              {
                                  s.round_total_ms = ctx->timestamp_ms - s.round_start_ms;
                                  const float total_sec = s.round_total_ms / 1000.0f;
                                  if (totalMax > 0.0f && total_sec > totalMax)
                                  {
                                      s.total_over = true;
                                      char d[160];
                                      snprintf(d, sizeof(d), "总耗时 %.1fs > %.1fs", total_sec, totalMax);
                                      s.total_detail = d;
                                      raise("sop_total_over");
                                  }
                                  if (totalMin > 0.0f && total_sec < totalMin)
                                  {
                                      s.total_short = true;
                                      char d[160];
                                      snprintf(d, sizeof(d), "总耗时 %.1fs < %.1fs", total_sec, totalMin);
                                      s.total_detail = d;
                                      raise("sop_total_short");
                                  }
                              }
                          }
                          s.ended = true;
    };
    const uint64_t now = ctx->timestamp_ms;
    if (target)
        s.last_seen_ms = now;

    /* ---- 工序结束①: 离场超时(leave 主判定 / endzone & trigger 兜底) ---- */
    if (!s.ended && s.last_seen_ms != 0 && !target && (now - s.last_seen_ms) > end_ms)
    {
        settle(); /* 最后所在步的 dwell_min 在 settle 内统一判, 不必这里重复 */
    }

    /* ---- 工序结束③: trigger 模式 — 收到外部结束触发信号即结算 ---- */
    if (!s.ended && useTriggerEnd && s.triggered_end)
        settle();

    /* ---- 进入防抖 + DAG 并行漫游 + 终点/新一轮 ---- */
    const bool external_mode = flow.trigger_mode == "external";
    if (!s.ended && target && (!external_mode || s.triggered))
    {
        /* 候选 step 解析: 在当前 cur_steps 的合法后继并集里, 找【全部】匹配 zone 的 step */
        const std::vector<int> nextCand = current_next();
        const std::vector<int> hits_now = inZone.empty() ? std::vector<int>() : match_steps(nextCand, inZone);

        /* 候选 zone 变化才重置防抖计时(命中集合相同的同 zone 仍按一次防抖处理) */
        if (inZone != s.cand_zone)
        {
            s.cand_zone = inZone;
            s.cand_since_ms = now;
        }
        /* 防抖确认时长: 命中集合内所有 step 的 enter_ms 取最大(保守, 确保最长的 enter 也满足) */
        uint64_t confirm_ms = 500;
        if (!hits_now.empty())
        {
            confirm_ms = 0;
            for (int h : hits_now)
                if (enter_ms[h] > confirm_ms)
                    confirm_ms = enter_ms[h];
        }

        if (!inZone.empty() && inZone != s.cur_zone && (now - s.cand_since_ms) >= confirm_ms)
        {
            if (!s.ended)
            {
                s.cur_zone = inZone;
                s.zone_history.push_back(inZone);

                /* 并行候选: 同一 zone 匹配多个合法后继(典型: 同 zone 多起点 / 同 zone 兄弟分支)时,
                 * 全部纳入 cur_steps 作为"候选路线"; 之后目标走出区分动作, 只有匹配的分支存活,
                 * 其余分支被悄悄放弃(见下方 judgeShort 跳过逻辑)→ 自然收敛到唯一路线。 */
                const std::vector<int> hits = match_steps(current_next(), inZone);

                if (!hits.empty()) /* 命中: 把所有匹配的候选 step 纳入新的 cur_steps */
                {
                    /* 离开旧 active step → 判其 dwell_min, 但【跳过被放弃的并行分支】:
                     * 只有"存在某个 hit 是 u 的后继"(即这条路线真的从 u 推进了)才 judgeShort(u);
                     * 否则 u 是一条没走通的候选分支, 它的短停留不算违规(避免误报"停留不足")。 */
                    for (int u : s.cur_steps)
                    {
                        bool advanced = false;
                        for (int h : hits)
                            if (std::find(succ[u].begin(), succ[u].end(), h) != succ[u].end())
                            {
                                advanced = true;
                                break;
                            }
                        if (advanced)
                            judgeShort(u);
                    }
                    /* 从 entry 首次推进 → 启动总耗时计时 */
                    if (s.cur_steps.empty() && s.round_start_ms == 0)
                        s.round_start_ms = now;
                    /* 循环边计数(普通边): 对每个 (旧 u, 新 h), 如果 u→h 是图上真实存在的边, 计数 +1 */
                    if (!loop_edges.empty())
                    {
                        for (int u : s.cur_steps)
                        {
                            for (int h : hits)
                            {
                                /* 必须是图上真实存在的边 */
                                if (std::find(succ[u].begin(), succ[u].end(), h) == succ[u].end())
                                    continue;
                                const int eidx = u * nseq + h;
                                if (tracked_loop_edge[eidx])
                                    s.edge_count_dense[eidx]++;
                            }
                        }
                        /* 自环计数(h→h 边): 语义 = "目标重新进入 h 的次数"
                         * 触发条件: 推进到 h 时, h 之前已被访问过(visited_steps[h] 已为 true)
                         *           说明 cur_step 从 h 切出去 又切回 h, 这就是用户画的"自环"事件。
                         * 注: 第一次进 h 不算自环 (因为没"回来"这个动作)。
                         * 用户必须画完整的"出+回"普通边 (如 A→B, B→A), 让状态机有切换路径,
                         * 自环 A→A 才能被触发 — 它是【额外的次数约束】, 不替代普通边。 */
                        for (int h : hits)
                        {
                            const int eidx = h * nseq + h;
                            if (s.visited_steps[h] && tracked_loop_edge[eidx])
                                s.edge_count_dense[eidx]++;
                        }
                    }
                    /* 撤销【被放弃的候选分支】: 旧 cur_steps 里没有任何 hit 是其后继的 step,
                     * 说明这条候选路线在本次推进被淘汰(典型: 两个同区域起点, 目标走出区分动作后
                     * 只剩一条)。把它的 visited / dwell / 计时全部回退, 避免画面上残留"假已完成 + 假计时"。
                     * (只在多候选收敛时触发: 单候选正常推进时 u 必然有 hit 后继, 不会被误撤销。) */
                    if (s.cur_steps.size() > 1)
                    {
                        for (int u : s.cur_steps)
                        {
                            bool advanced = false;
                            for (int h : hits)
                                if (std::find(succ[u].begin(), succ[u].end(), h) != succ[u].end())
                                {
                                    advanced = true;
                                    break;
                                }
                            if (!advanced && std::find(hits.begin(), hits.end(), u) == hits.end())
                            {
                                if (s.visited_steps[u])
                                {
                                    s.visited_steps[u] = false;
                                    if (s.expect > 0)
                                        s.expect--;
                                }
                                s.dwell_ms[u] = 0;
                                s.short_done[u] = false;
                                s.long_done[u] = false;
                            }
                        }
                    }

                    /* 推进: cur_steps 替换为命中集合; 全部标记 visited */
                    s.cur_steps = hits;
                    for (int h : hits)
                    {
                        if (!s.visited_steps[h])
                        {
                            s.visited_steps[h] = true;
                            s.expect++;
                        }
                    }
                    /* "完成"判定:
                     *   exits 非空 → 命中集合中任一是 exit step → 标记 completed(走通一条到出口的路径);
                     *   exits 空 → 退化为"全 visited 才算 completed"。
                     * 注: completed 只是"路径走通"标记, 不直接触发 settle —— 工序结束完全由
                     *   leave 模式 → 离场超时 reset_sec;
                     *   endzone 模式 → 进入终点 zone(兜底离场超时);
                     * 决定。这样: 用户画环图(如 step3 走完后回到 step1)时, 进入 exit step 不会
                     * 强制结束工序, 目标可以继续走环, 直到真正离场才 settle。 */
                    bool reached = false;
                    if (!exits.empty())
                    {
                        for (int h : hits)
                            if (std::find(exits.begin(), exits.end(), h) != exits.end())
                            {
                                reached = true;
                                break;
                            }
                    }
                    else
                    {
                        reached = (s.expect >= nseq);
                    }
                    if (reached)
                        s.completed = true;
                }
                else if (is_designed_zone(inZone)) /* 进了"设计内 zone"但所有 active 后继中都没匹配 → 顺序错误 */
                {
                    if (!s.order_error)
                    {
                        std::string expectList;
                        const std::vector<int> nx = current_next();
                        for (size_t k = 0; k < nx.size(); ++k)
                        {
                            if (k)
                                expectList += ",";
                            expectList += seq[nx[k]];
                        }
                        s.err_detail = "进了[" + inZone + "] 当前可走[" +
                                       (expectList.empty() ? std::string("-") : expectList) + "]";
                        raise("sop_order_err");
                    }
                    s.order_error = true;
                }
            }
        }
    }

    /* 外部触发必须模式: 未触发但目标进入了设计区域 → 违规(latch, 本轮一次) */
    if (external_mode && !s.triggered && target && !inZone.empty() && is_designed_zone(inZone))
    {
        const bool mandatory = flow.trigger_mandatory;
        if (mandatory && !s.untriggered_entry)
        {
            s.untriggered_entry = true;
            s.err_detail = "未经触发进入[" + inZone + "], 缺少外部触发信号";
            raise("sop_untracked_entry");
        }
    }

    /* 停留计时: 对每一个 active step 独立累加 dwell, 各自判 dwell_max。
     * 【每次进入单独计时】: 目标离开该步区域超过宽限后再回来 → 本次访问重新计时(dwell 清零),
     * 而不是接着上一次的累计。宽限 = max(进入确认时长, 300ms), 用来过滤检测抖动的一两帧离场。 */
    if (target && !s.ended && !s.cur_steps.empty())
    {
        for (int st : s.cur_steps)
        {
            if (st < 0 || st >= nseq)
                continue;
            const bool inThis = !inZone.empty() && (seq[st] == inZone);
            if (!inThis)
            {
                /* 目标不在该步区域: 记下离开时刻(只记第一帧) */
                if (s.dwell_out_since[st] == 0)
                    s.dwell_out_since[st] = now;
                continue;
            }
            /* 目标在该步区域内 */
            if (s.dwell_out_since[st] != 0)
            {
                const uint64_t grace = std::max<uint64_t>(enter_ms[st], 300);
                if ((now - s.dwell_out_since[st]) >= grace)
                {
                    /* 离开够久又回来 → 这是新一次进入 → 本次重新计时 */
                    s.dwell_ms[st] = 0;
                    s.short_done[st] = false;
                    s.long_done[st] = false;
                }
                s.dwell_out_since[st] = 0;
            }
            s.dwell_ms[st] += (uint64_t)std::max(0.0f, ctx->dt_ms);
            if (dwell_max[st] > 0.0f && !s.long_done[st])
            {
                const uint64_t cap = (uint64_t)(dwell_max[st] * 1000.0f + 0.5f);
                if (s.dwell_ms[st] > cap)
                {
                    s.long_done[st] = true;
                    s.dwell_over = true;
                    char d[160];
                    snprintf(d, sizeof(d), "步%d[%s] 停留%.1fs > %.1fs", st + 1, seq[st].c_str(),
                             s.dwell_ms[st] / 1000.0f, dwell_max[st]);
                    s.dwell_detail = d;
                    raise("sop_dwell_over");
                }
            }
        }
    }

    /* ---- 工序结束②: 终点区域连续停留达到阈值 ----
     * 计时从目标实际进入终点 ROI 开始；任何一帧离开终点或目标丢失都会清零。
     * 同时等待通用“进入确认”把终点写入 cur_zone，保证终点若也是 SOP 步骤，路径推进、
     * completed 和停留统计已经更新后再结算。阈值为 0 时保持旧配置行为。 */
    if (!s.ended && useEndZone)
    {
        if (target && inZone == endZone)
        {
            if (!s.end_zone_tracking || s.tracked_end_zone != endZone)
            {
                s.end_zone_tracking = true;
                s.tracked_end_zone = endZone;
                s.end_zone_since_ms = now;
            }
            const uint64_t stayed_ms = now >= s.end_zone_since_ms ? now - s.end_zone_since_ms : 0;
            if (s.cur_zone == endZone && stayed_ms >= end_dwell_ms)
                settle();
        }
        else
        {
            s.end_zone_tracking = false;
            s.tracked_end_zone.clear();
            s.end_zone_since_ms = 0;
        }
    }
    else if (!useEndZone)
    {
        s.end_zone_tracking = false;
        s.tracked_end_zone.clear();
        s.end_zone_since_ms = 0;
    }

    /* ===================== 画面绘制 ===================== */
    const cv::Scalar GREEN(0, 238, 0), RED(0, 0, 230), ORANGE(0, 165, 255), GRAY(170, 170, 170), CYAN(255, 255, 0),
        WHITE(255, 255, 255), DGREEN(0, 180, 0);

    /* 1) 各区域: 目标所在=绿粗 / 当前可走(后继 + 有自环的当前步自身)=青 / 已访问=深绿 / 未到=黄 */
    std::vector<std::string> nextZones; /* "可走"区域集合 */
    if (!s.ended)
    {
        for (int sx : current_next())
            nextZones.push_back(seq[sx]);
        /* 有自环的当前步: 它自己的区域也算"可走"(可离开后重进) → 高亮成青色 */
        for (int st : s.cur_steps)
            if (st >= 0 && st < nseq && self_loop[st])
                nextZones.push_back(seq[st]);
    }
    auto isExpectZone = [&](const std::string &n) -> bool {
        for (const auto &x : nextZones)
            if (x == n)
                return true;
        return false;
    };
    if (ctx->rois)
        for (const auto &z : *ctx->rois)
        {
            if (z.polygon.size() < 3)
                continue;
            std::string refs;
            for (int i = 0; i < nseq; ++i)
                if (seq[i] == z.name)
                {
                    if (!refs.empty())
                        refs += ",";
                    refs += std::to_string(i + 1);
                }
            const bool hasTarget = target && cv::pointPolygonTest(z.polygon, center, false) >= 0.0;
            const bool isExpect = isExpectZone(z.name);
            const cv::Scalar col = hasTarget  ? GREEN
                                   : isExpect ? CYAN
                                              : (inVisited(z.name) ? DGREEN : cv::Scalar(0, 255, 255));
            draw_polyline(ctx, z.polygon, col, hasTarget ? 4 : (isExpect ? 3 : 2), 1.0, true);
            char tag[128];
            snprintf(tag, sizeof(tag), "%s%s 步%s", z.name.c_str(), (useEndZone && z.name == endZone) ? "[终点]" : "",
                     refs.empty() ? "-" : refs.c_str());
            const cv::Point a(z.polygon.front().x + 4, std::max(14, z.polygon.front().y - 6));
            draw_text(ctx, tag, a, col, 0.5, 1);
        }

    /* 2) 目标框/中心 */
    if (target)
    {
        target->box_color = cv::Scalar(0, 200, 0);
        draw_circle(ctx, center, 4, cv::Scalar(0, 200, 0), 2);
    }

    /* 3) 右上"四行合规状态"(始终显示, 合规=绿/违规=红): 一种报警单独一行。
     *    工序结束并 reset_state() 后, 四行自动全部回到绿色(无需额外清理)。
     *    漏检在工序结束(s.ended)前一律绿色 —— 此时还无法判定有没有漏。 */
    const int W = ctx->frame ? ctx->frame->cols : 640;
    const int x_right = std::max(180, W - 200); /* 右上起始列, 留 200px 给状态行 */
    int yrow_r = 28;
    const cv::Scalar STATUS_GREEN(0, 220, 80);
    const cv::Scalar STATUS_RED(0, 30, 230);

    auto status_row = [&](bool ok, const char *okTxt, const char *badTxt, const std::string &detail) {
        draw_text(ctx, ok ? okTxt : badTxt, cv::Point(x_right, yrow_r), ok ? STATUS_GREEN : STATUS_RED, 0.7, 2);
        yrow_r += 28;
        if (!ok && !detail.empty())
        { /* 违规详情: 小字, 单独一行 */
            draw_text(ctx, detail.c_str(), cv::Point(x_right, yrow_r), STATUS_RED, 0.42, 1);
            yrow_r += 22;
        }
    };

    /* 第1行 顺序 */
    status_row(!s.order_error, "顺序正常", "顺序错误", s.err_detail);

    /* 第2行 漏检: 只在结束后才能判, 未结束保持绿 */
    std::string missDetail;
    if (s.ended && !s.missed.empty())
    {
        for (size_t k = 0; k < s.missed.size(); ++k)
        {
            if (k)
                missDetail += ",";
            missDetail += s.missed[k];
        }
        missDetail = "缺少: " + missDetail;
    }
    const bool noMiss = !s.ended || s.missed.empty();
    status_row(noMiss, "无遗漏区域", "存在遗漏区域", missDetail);

    /* 第3行 停留超时 */
    status_row(!s.dwell_over, "停留时长正常", "停留超时", s.dwell_over ? s.dwell_detail : std::string());

    /* 第4行 停留不足 */
    status_row(!s.dwell_short, "停留时长达标", "停留不足", s.dwell_short ? s.dwell_detail : std::string());

    /* 第5行 总耗时(配了上限或下限才显示) */
    if (totalMin > 0.0f || totalMax > 0.0f)
    {
        const bool totalOk = !s.total_over && !s.total_short;
        const char *totalBad = s.total_over ? "总耗时超时" : (s.total_short ? "总耗时不足" : "总耗时异常");
        status_row(totalOk, "总耗时正常", totalBad, totalOk ? std::string() : s.total_detail);
    }

    /* 第6行 循环次数(配了任何边 limit 才显示) */
    if (!limited_edges.empty())
    {
        status_row(!s.loop_violation, "循环次数正常", "循环次数不符", s.loop_violation ? s.loop_detail : std::string());
    }

    /* 4) 左上信息: 目标在线/区域、进度、下一步(报警挪到右上后, 左上只保留这些指示信息) */
    int yrow = 28;
    char line[160];

    /* 按【路线】编号(每个 entry=第1步, 沿 succ BFS +1) + 当前活跃路线的可达集。
     * 用于: 进度 N/M 的 M 取活跃路线步数; 步骤清单只显示活跃路线 + 按路线序号。 */
    std::vector<int> route_no(nseq, 0);
    {
        std::vector<std::pair<int, int>> q;
        for (int e : entries)
            q.push_back({e, 1});
        size_t qi = 0;
        while (qi < q.size())
        {
            const int u = q[qi].first, lvl = q[qi].second;
            ++qi;
            if (u < 0 || u >= nseq)
                continue;
            if (route_no[u] != 0 && route_no[u] <= lvl)
                continue;
            route_no[u] = lvl;
            for (int v : succ[u])
                q.push_back({v, lvl + 1});
        }
        for (int i = 0; i < nseq; ++i)
            if (route_no[i] == 0)
                route_no[i] = i + 1;
    }
    bool any_visited = false;
    for (int i = 0; i < nseq; ++i)
        if (s.visited_steps[i])
        {
            any_visited = true;
            break;
        }
    const bool running = any_visited || !s.cur_steps.empty();
    std::vector<bool> relevant(nseq, false);
    if (running)
    {
        for (int i = 0; i < nseq; ++i)
            if (s.visited_steps[i])
                relevant[i] = true;
        std::vector<int> st(s.cur_steps.begin(), s.cur_steps.end());
        std::vector<bool> seen(nseq, false);
        while (!st.empty())
        {
            const int u = st.back();
            st.pop_back();
            if (u < 0 || u >= nseq || seen[u])
                continue;
            seen[u] = true;
            relevant[u] = true;
            for (int v : succ[u])
                st.push_back(v);
        }
    }
    else
    {
        for (int e : entries)
            if (e >= 0 && e < nseq)
                relevant[e] = true;
    }
    /* 未开工: 只提示从哪个起点进 (不堆所有路线步骤, 否则看不出分支) */
    if (!running && !s.ended)
    {
        std::string entryZones;
        for (int e : entries)
        {
            if (e < 0 || e >= nseq)
                continue;
            if (entryZones.find(seq[e]) != std::string::npos)
                continue; /* 粗去重 */
            if (!entryZones.empty())
                entryZones += " / ";
            entryZones += seq[e];
        }
        snprintf(line, sizeof(line), "进入起点开始: %s", entryZones.c_str());
        draw_text(ctx, line, cv::Point(16, yrow), CYAN, 0.6, 2);
        yrow += 28;
        if (external_mode)
        {
            if (s.triggered)
                draw_text(ctx, "已触发, 等待目标进入起点...", cv::Point(16, yrow), cv::Scalar(0, 255, 255), 0.65, 2);
            else if (s.untriggered_entry)
                draw_text(ctx, "违规: 未经触发进入区域", cv::Point(16, yrow), cv::Scalar(0, 0, 230), 0.7, 2);
            else
                draw_text(ctx, "等待外部触发...", cv::Point(16, yrow), cv::Scalar(0, 165, 255), 0.7, 2);
            yrow += 28;
        }
    }
    /* 合规完成一行 (违规细节在右上 6 行, 这里不重复) */
    else if (s.ended && !s.order_error && s.missed.empty() && !s.dwell_short && !s.dwell_over && !s.total_over &&
             !s.total_short && !s.loop_violation)
    {
        draw_text(ctx, "工序合规完成", cv::Point(16, yrow), GREEN, 0.8, 2);
        yrow += 30;
    }

    /* 进行中: 只列【当前活跃路线】的步骤, 每行尽量短:
     *   完成=绿「完成」/ 停留不足=橙;  当前所在=亮青「当前」+实时停留(配了要求才显示区间);
     *   可走=青「可走」;  待经过=灰(只显示区域名, 不加字)。 */
    if (running)
    {
        const int hmax = (ctx->frame ? ctx->frame->rows : 640) - 16;
        const std::vector<int> nextNow = current_next();
        auto in_set = [](const std::vector<int> &v, int i) -> bool {
            for (int x : v)
                if (x == i)
                    return true;
            return false;
        };

        for (int i = 0; i < nseq; ++i)
        {
            if (!relevant[i])
                continue;
            if (yrow > hmax)
            {
                draw_text(ctx, "...", cv::Point(16, yrow), GRAY, 0.5, 1);
                break;
            }
            const float dsec = s.dwell_ms[i] / 1000.0f;
            const bool done = s.visited_steps[i];
            const bool cur = in_set(s.cur_steps, i) && !s.ended;
            const bool reach = !done && !cur && !s.ended && in_set(nextNow, i);
            const bool hasReq = (dwell_min[i] > 0.0f || dwell_max[i] > 0.0f);
            cv::Scalar c;
            const char *mk;
            char extra[48];
            extra[0] = '\0';
            if (done)
            {
                const bool ok = (dwell_min[i] <= 0.0f) || (dsec + 1e-3f >= dwell_min[i]);
                c = ok ? GREEN : ORANGE;
                mk = ok ? "完成" : "停留不足";
            }
            else if (cur)
            {
                c = CYAN;
                mk = "当前";
                if (hasReq && dwell_min[i] > 0.0f && dwell_max[i] > 0.0f)
                    snprintf(extra, sizeof(extra), " %.1fs(%.0f~%.0f)", dsec, dwell_min[i], dwell_max[i]);
                else if (dwell_min[i] > 0.0f)
                    snprintf(extra, sizeof(extra), " %.1fs(>=%.0f)", dsec, dwell_min[i]);
                else if (dwell_max[i] > 0.0f)
                    snprintf(extra, sizeof(extra), " %.1fs(<=%.0f)", dsec, dwell_max[i]);
                else
                    snprintf(extra, sizeof(extra), " %.1fs", dsec);
            }
            else if (reach)
            {
                c = CYAN;
                mk = "可走";
            }
            else
            {
                c = GRAY;
                mk = "";
            }
            snprintf(line, sizeof(line), "%d.%s%s %s", route_no[i], seq[i].c_str(), extra, mk);
            draw_text(ctx, line, cv::Point(16, yrow), c, 0.55, cur ? 2 : 1);
            yrow += 22;
        }
    }

    /* 5) 左下角: 工序总耗时实时显示 (round_start_ms==0 表示本轮未启动 → "待开始")。
     *    工序进行中 → 青色;结束并合规 → 绿;结束并违规上下限 → 红。 */
    {
        const int H = ctx->frame ? ctx->frame->rows : 640;
        const int y_bottom = H - 18;
        char buf[96];
        cv::Scalar tc = CYAN;
        if (s.round_start_ms == 0)
        {
            snprintf(buf, sizeof(buf), "总耗时: 待开始");
            tc = GRAY;
        }
        else if (!s.ended)
        {
            const uint64_t live = (now > s.round_start_ms) ? (now - s.round_start_ms) : 0;
            snprintf(buf, sizeof(buf), "总耗时: %.1fs (进行中)", live / 1000.0f);
            tc = CYAN;
        }
        else
        {
            const bool ok = !s.total_over && !s.total_short;
            snprintf(buf, sizeof(buf), "总耗时: %.1fs", s.round_total_ms / 1000.0f);
            tc = ok ? GREEN : RED;
        }
        draw_text(ctx, buf, cv::Point(16, y_bottom), tc, 0.6, 2);
    }

    /* ===== 通用告警入口 =====
     * SOP只描述事件和运行时字段；保存什么媒体、投递到哪里、JSON如何映射均由本通道
     * report_policy 决定。相同通道+相同报警类型5秒内由框架自动合并。 */
    auto make_event_payload = [&](bool normal, const std::string &message) -> std::string {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "event_type", "sop_result");
        cJSON_AddStringToObject(root, "edge_verdict", normal ? "normal" : "violation");
        cJSON_AddStringToObject(root, "message", message.c_str());

        const uint64_t total_ms =
            s.round_start_ms == 0
                ? 0
                : (s.ended ? s.round_total_ms : (now > s.round_start_ms ? now - s.round_start_ms : 0));
        cJSON *sop = cJSON_CreateObject();
        cJSON_AddStringToObject(sop, "target_label", targetLab.c_str());
        cJSON_AddBoolToObject(sop, "completed", s.completed ? 1 : 0);
        cJSON_AddStringToObject(sop, "current_zone", s.cur_zone.c_str());
        cJSON_AddNumberToObject(sop, "progress", s.expect);
        cJSON_AddNumberToObject(sop, "step_count", nseq);
        cJSON_AddNumberToObject(sop, "round_total_seconds", total_ms / 1000.0);
        cJSON_AddStringToObject(sop, "end_mode", endMode.c_str());
        cJSON_AddStringToObject(sop, "end_zone", endZone.c_str());
        cJSON_AddNumberToObject(sop, "required_end_dwell_seconds", endDwellSec);
        const uint64_t end_stay_ms = s.end_zone_tracking && now >= s.end_zone_since_ms ? now - s.end_zone_since_ms : 0;
        cJSON_AddNumberToObject(sop, "actual_end_zone_stay_seconds", end_stay_ms / 1000.0);
        cJSON_AddItemToObject(sop, "configured_sequence", path_string_array(seq));
        cJSON_AddItemToObject(sop, "zone_history", path_string_array(s.zone_history));

        cJSON *configured_edges = cJSON_CreateArray();
        for (int src = 0; src < nseq; ++src)
        {
            for (int dst = 0; dst < nseq; ++dst)
            {
                const bool is_self = src == dst && self_loop[src];
                const bool is_normal =
                    src != dst && std::find(succ[src].begin(), succ[src].end(), dst) != succ[src].end();
                if (!is_self && !is_normal)
                    continue;
                const int index = src * nseq + dst;
                cJSON *edge = cJSON_CreateObject();
                cJSON_AddNumberToObject(edge, "from_step_index", src);
                cJSON_AddNumberToObject(edge, "to_step_index", dst);
                cJSON_AddStringToObject(edge, "from_zone", seq[src].c_str());
                cJSON_AddStringToObject(edge, "to_zone", seq[dst].c_str());
                cJSON_AddBoolToObject(edge, "is_self_loop", is_self ? 1 : 0);
                cJSON_AddBoolToObject(edge, "is_loop_edge", tracked_loop_edge[index] ? 1 : 0);
                cJSON_AddItemToArray(configured_edges, edge);
            }
        }
        cJSON_AddItemToObject(sop, "configured_edges", configured_edges);

        auto configured_step_refs = [&](const std::vector<int> &steps) -> cJSON * {
            cJSON *items = cJSON_CreateArray();
            for (int step : steps)
            {
                if (step < 0 || step >= nseq)
                    continue;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "step_index", step);
                cJSON_AddNumberToObject(item, "step_number", step + 1);
                cJSON_AddStringToObject(item, "zone", seq[step].c_str());
                cJSON_AddItemToArray(items, item);
            }
            return items;
        };
        cJSON_AddItemToObject(sop, "configured_entry_steps", configured_step_refs(entries));
        cJSON_AddItemToObject(sop, "configured_exit_steps", configured_step_refs(exits));

        /* 循环信息无论正常或违规都输出；两组字段刻意分开，便于 Dify/业务端分别读取配置与实绩。
         * required_min_count / allowed_max_count 为 0 表示该侧不限，两端均为 0 即任意次数。 */
        cJSON *configured_loop_edges = cJSON_CreateArray();
        cJSON *actual_loop_counts = cJSON_CreateArray();
        for (const auto &edge : loop_edges)
        {
            const int index = edge.first * nseq + edge.second;
            const int minimum = limit_min[index];
            const int maximum = limit_max[index];
            const int actual = s.edge_count_dense[index];

            cJSON *configured = cJSON_CreateObject();
            cJSON_AddNumberToObject(configured, "from_step_index", edge.first);
            cJSON_AddNumberToObject(configured, "to_step_index", edge.second);
            cJSON_AddStringToObject(configured, "from_zone", seq[edge.first].c_str());
            cJSON_AddStringToObject(configured, "to_zone", seq[edge.second].c_str());
            cJSON_AddNumberToObject(configured, "required_min_count", minimum);
            cJSON_AddNumberToObject(configured, "allowed_max_count", maximum);
            cJSON_AddBoolToObject(configured, "range_unlimited", minimum == 0 && maximum == 0 ? 1 : 0);
            cJSON_AddItemToArray(configured_loop_edges, configured);

            const bool within_range = (minimum == 0 || actual >= minimum) && (maximum == 0 || actual <= maximum);
            cJSON *runtime = cJSON_CreateObject();
            cJSON_AddNumberToObject(runtime, "from_step_index", edge.first);
            cJSON_AddNumberToObject(runtime, "to_step_index", edge.second);
            cJSON_AddStringToObject(runtime, "from_zone", seq[edge.first].c_str());
            cJSON_AddStringToObject(runtime, "to_zone", seq[edge.second].c_str());
            cJSON_AddNumberToObject(runtime, "actual_count", actual);
            cJSON_AddBoolToObject(runtime, "within_range", within_range ? 1 : 0);
            cJSON_AddItemToArray(actual_loop_counts, runtime);
        }
        cJSON_AddItemToObject(sop, "configured_loop_edges", configured_loop_edges);
        cJSON_AddItemToObject(sop, "actual_loop_counts", actual_loop_counts);
        cJSON_AddItemToObject(root, "sop", sop);

        cJSON *alarm_types = cJSON_CreateObject();
        auto add_group = [&](const char *key, const char *name, const std::string &detail) -> cJSON * {
            cJSON *group = cJSON_CreateObject();
            cJSON_AddStringToObject(group, "name", name);
            cJSON_AddStringToObject(group, "message", detail.c_str());
            cJSON *items = cJSON_CreateArray();
            cJSON_AddItemToObject(group, "items", items);
            cJSON_AddItemToObject(alarm_types, key, group);
            return items;
        };

        if (normal)
        {
            cJSON *items = add_group("normal", "正常完成", message);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddBoolToObject(item, "completed", 1);
            cJSON_AddNumberToObject(item, "round_total_seconds", total_ms / 1000.0);
            cJSON_AddItemToArray(items, item);
        }
        if (s.order_error)
        {
            cJSON *items = add_group("order_error", "顺序错误", s.err_detail);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "entered_zone", s.cur_zone.c_str());
            cJSON_AddStringToObject(item, "detail", s.err_detail.c_str());
            std::vector<std::string> expected_zones;
            for (int step : current_next())
                if (step >= 0 && step < nseq &&
                    std::find(expected_zones.begin(), expected_zones.end(), seq[step]) == expected_zones.end())
                    expected_zones.push_back(seq[step]);
            cJSON_AddItemToObject(item, "expected_zones", path_string_array(expected_zones));
            cJSON_AddItemToObject(item, "zone_history", path_string_array(s.zone_history));
            cJSON_AddItemToArray(items, item);
        }
        if (!s.missed.empty())
        {
            cJSON *items = add_group("missed_step", "遗漏工序", "工序结束时存在未完成区域");
            cJSON *item = cJSON_CreateObject();
            cJSON_AddItemToObject(item, "missed_zones", path_string_array(s.missed));
            cJSON_AddItemToObject(item, "zone_history", path_string_array(s.zone_history));
            cJSON_AddItemToArray(items, item);
        }
        if (s.dwell_short)
        {
            cJSON *items = add_group("dwell_short", "停留时间不足", s.dwell_detail);
            for (int i = 0; i < nseq; ++i)
            {
                if (!s.short_done[i] || dwell_min[i] <= 0.0f)
                    continue;
                const double actual = s.dwell_ms[i] / 1000.0;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "step_index", i);
                cJSON_AddNumberToObject(item, "step_number", i + 1);
                cJSON_AddStringToObject(item, "zone", seq[i].c_str());
                cJSON_AddNumberToObject(item, "actual_seconds", actual);
                cJSON_AddNumberToObject(item, "required_min_seconds", dwell_min[i]);
                cJSON_AddNumberToObject(item, "shortage_seconds", std::max(0.0, dwell_min[i] - actual));
                cJSON_AddItemToArray(items, item);
            }
        }
        if (s.dwell_over)
        {
            cJSON *items = add_group("dwell_over", "停留时间过长", s.dwell_detail);
            for (int i = 0; i < nseq; ++i)
            {
                if (!s.long_done[i] || dwell_max[i] <= 0.0f)
                    continue;
                const double actual = s.dwell_ms[i] / 1000.0;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "step_index", i);
                cJSON_AddNumberToObject(item, "step_number", i + 1);
                cJSON_AddStringToObject(item, "zone", seq[i].c_str());
                cJSON_AddNumberToObject(item, "actual_seconds", actual);
                cJSON_AddNumberToObject(item, "allowed_max_seconds", dwell_max[i]);
                cJSON_AddNumberToObject(item, "excess_seconds", std::max(0.0, actual - dwell_max[i]));
                cJSON_AddItemToArray(items, item);
            }
        }
        if (s.total_short)
        {
            cJSON *items = add_group("total_short", "总耗时不足", s.total_detail);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "actual_total_seconds", total_ms / 1000.0);
            cJSON_AddNumberToObject(item, "required_min_seconds", totalMin);
            cJSON_AddNumberToObject(item, "allowed_max_seconds", totalMax);
            cJSON_AddItemToArray(items, item);
        }
        if (s.total_over)
        {
            cJSON *items = add_group("total_over", "总耗时过长", s.total_detail);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "actual_total_seconds", total_ms / 1000.0);
            cJSON_AddNumberToObject(item, "required_min_seconds", totalMin);
            cJSON_AddNumberToObject(item, "allowed_max_seconds", totalMax);
            cJSON_AddItemToArray(items, item);
        }
        if (s.loop_violation)
        {
            cJSON *items = add_group("loop_violation", "循环次数不符", s.loop_detail);
            for (const auto &edge : limited_edges)
            {
                const int index = edge.first * nseq + edge.second;
                const int count = s.edge_count_dense[index];
                const int minimum = limit_min[index], maximum = limit_max[index];
                if (!((minimum > 0 && count < minimum) || (maximum > 0 && count > maximum)))
                    continue;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "from_step_index", edge.first);
                cJSON_AddNumberToObject(item, "to_step_index", edge.second);
                cJSON_AddStringToObject(item, "from_zone", seq[edge.first].c_str());
                cJSON_AddStringToObject(item, "to_zone", seq[edge.second].c_str());
                cJSON_AddNumberToObject(item, "actual_count", count);
                cJSON_AddNumberToObject(item, "required_min_count", minimum);
                cJSON_AddNumberToObject(item, "allowed_max_count", maximum);
                cJSON_AddItemToArray(items, item);
            }
        }
        if (s.untriggered_entry)
        {
            cJSON *items = add_group("untriggered_entry", "未经触发进入工序", s.err_detail);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "entered_zone", s.cur_zone.c_str());
            cJSON_AddStringToObject(item, "trigger_mode", "external");
            cJSON_AddBoolToObject(item, "trigger_received", s.triggered ? 1 : 0);
            cJSON_AddItemToArray(items, item);
        }
        cJSON_AddItemToObject(root, "alarm_type", alarm_types);
        return path_json_text(root);
    };

    auto report_sop_result = [&](const std::string &type, const std::string &message, bool normal) {
        EventRequest request;
        request.event_type = type;
        request.message = message;
        request.merge_mode = EventMergeMode::NEVER;
        request.fields.set_string("current_zone", s.cur_zone);
        request.fields.set_number("progress", s.expect);
        request.fields.set_number("step_count", nseq);
        request.fields.set_number("round_total_seconds", s.round_total_ms / 1000.0);
        request.fields.set_bool("order_error", s.order_error);
        request.fields.set_bool("completed", s.completed);
        request.fields.set_string("edge_verdict", normal ? "normal" : "violation");
        request.fields.set_json("event_payload", make_event_payload(normal, message));
        const EventReportResult report = report_event(ctx, request);
        if (!report.accepted())
            fprintf(stderr, "[logic_path_sop][ch%02d] report rejected: status=%s detail=%s\n", ctx->chnId,
                    event_report_status_name(report.status), report.detail.c_str());
    };

    if (!alarms.empty())
    {
        std::vector<std::string> names;
        if (s.order_error)
            names.push_back("顺序错误");
        if (!s.missed.empty())
            names.push_back("遗漏工序");
        if (s.dwell_short)
            names.push_back("停留时间不足");
        if (s.dwell_over)
            names.push_back("停留时间过长");
        if (s.total_short)
            names.push_back("总耗时不足");
        if (s.total_over)
            names.push_back("总耗时过长");
        if (s.loop_violation)
            names.push_back("循环次数不符");
        if (s.untriggered_entry)
            names.push_back("未经触发进入工序");
        std::string message = "SOP工序违规";
        if (!names.empty())
        {
            message += ": ";
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (i)
                    message += ",";
                message += names[i];
            }
        }
        report_sop_result("sop_violation", message, false);
    }

    /* 正常结果只能在 settle 已完成后产生。round_start_ms 防止空轮次/未触发离场被误报，
     * completed 保证确实走到合法出口；所有 latch 都检查，覆盖此前帧已上报过的即时违规。 */
    const bool normal_result = s.ended && s.round_start_ms != 0 && s.completed && alarms.empty() && !s.order_error &&
                               s.missed.empty() && !s.dwell_short && !s.dwell_over && !s.total_short && !s.total_over &&
                               !s.loop_violation && !s.untriggered_entry;
    if (flow.report_normal && normal_result)
    {
        report_sop_result("sop_normal", "SOP工序正常完成", true);
    }

    /* ===== 工序结束后立刻把所有 runtime + 显示用 latch 清零 =====
     * 这一帧已经完成报警上报 + 叠加图渲染(含报警红字, 上报记录里能查), 下一帧屏幕完全归零:
     *   右上 6 行回到全绿, 左下"总耗时: 待开始", 步骤列表全灰, "工序合规完成" 不再显示。
     * 等待目标重新进入任何 zone 自然触发新一轮(reset_state 已把 ended 清成 false)。
     *
     * 关键: reset_state 后把 cur_zone 重新设为当前 inZone, 防止"目标仍停在同一区域被当成
     * 新动作"反复触发 settle —— 典型场景: endzone 模式下目标提前进终点 settle 报漏检, 然后
     * 目标继续待在终点区域不动, 如果 cur_zone 被清空, 下一帧 inZone(终点) != cur_zone(空)
     * 会再次走防抖+endzone 判定 → 又 settle → 又报警 → 死循环每帧报漏检。
     * 把 cur_zone 设为 inZone 后, 下次防抖触发的前提是"目标移到一个新区域", 才合理开新一轮。 */
    if (s.ended)
    {
        reset_state();
        s.cur_zone = inZone; /* leave 模式 target 离场时 inZone="", 自然不影响目标回来 */
    }
}

REGISTER_LOGIC(logic_path_sop);
REGISTER_LOGIC_ACTION(logic_path_sop, logic_path_sop_action);
