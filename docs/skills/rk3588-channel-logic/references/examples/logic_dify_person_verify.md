# logic_dify_person_verify — 人员 Dify 去重核验

- **上报**：dify（`dify_uploader_enqueue`，带标注图）
- **可调参数**：无（间隔/容忍帧数写死）
- **用到的能力**：**按 track_id 去重**、丢帧容忍、ROI 过滤、Dify 上报、上报图带框

## 做什么
画面出现**未上报过的人员**时发一次 Dify 核验，靠 tracker 的 `track_id` 防止同一人重复报。去重策略：
- 每帧收集 ROI 内、`track_id >= 0` 的 person 的 id；
- 不在 `reported_ids` 里的新 id → 触发上报；
- id 连续丢失超过 `MISS_FRAME_MAX(30)` 帧才从记录中清除（容忍短暂遮挡/漏检）；
- 两次上报至少间隔 `MIN_INTERVAL_MS(5000)`。

## 完整实现（核心去重逻辑完整；上报图的逐框标注做了精简注释）
```cpp
static void logic_dify_person_verify(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty()) return;
    if (!ctx->frame || ctx->frame->empty()) return;
    if (!*ctx->state) *ctx->state = std::make_shared<DifyPersonVerifyState>();
    auto &s = *std::static_pointer_cast<DifyPersonVerifyState>(*ctx->state);

    static constexpr uint64_t MIN_INTERVAL_MS = 5000;
    static constexpr int MISS_FRAME_MAX = 30;

    if (s.first) { s.last_upload_ms = ctx->timestamp_ms; s.first = 0; return; }

    /* 1) 收集本帧 ROI 内、有 track_id 的 person */
    std::set<int> current_person_ids;
    for (auto &r : *ctx->results) {
        if (r.label != "person") continue;
        if (r.track_id < 0) continue;
        if (!roi_contains(ctx, r.box, ROI_ALL)) continue;   // 没画 ROI=整帧不设限
        current_person_ids.insert(r.track_id);
        r.box_color = cv::Scalar(0,0,255);
    }

    /* 2) 更新丢帧计数：在画面的清零；不在的累加，超阈值则从 reported/miss 清除 */
    for (int tid : current_person_ids) s.miss_frames[tid] = 0;
    {
        std::vector<int> expired;
        for (auto &kv : s.miss_frames) {
            if (current_person_ids.count(kv.first)) continue;
            if (++kv.second > MISS_FRAME_MAX) expired.push_back(kv.first);
        }
        for (int tid : expired) { s.miss_frames.erase(tid); s.reported_ids.erase(tid); }
    }

    /* 3) 找一个没报过的新 id */
    bool has_new_person = false; int new_tid = -1;
    for (int tid : current_person_ids)
        if (!s.reported_ids.count(tid)) { has_new_person = true; new_tid = tid; break; }

    /* 4) 限频后上报 Dify */
    if (has_new_person && (ctx->timestamp_ms - s.last_upload_ms >= MIN_INTERVAL_MS)) {
        const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                                 ? ctx->config->dify_prompt.c_str() : "person detected, please verify";
        char event_id[128];
        snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu_person",
                 ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);

        cv::Mat upload_img = ctx->frame->clone();
        // ... 在 upload_img 上逐框画 矩形 + "id:N label score" 半透明标签（cv::rectangle/putText）...

        const char *dify_url = (ctx->config && !ctx->config->dify_api_url.empty()) ? ctx->config->dify_api_url.c_str() : nullptr;
        const char *dify_key = (ctx->config && !ctx->config->dify_api_key.empty()) ? ctx->config->dify_api_key.c_str() : nullptr;
        dify_uploader_enqueue(upload_img, prompt, event_id, dify_url, dify_key);
        s.last_upload_ms = ctx->timestamp_ms;

        /* 把当前可见 id 全标记已报，防同帧多人轮流触发 */
        for (int tid : current_person_ids) s.reported_ids.insert(tid);
    }
}
```
> 完整逐框标注绘制见源码 `src/logic/logic_dify_person_verify.cpp` 的同名函数。跨帧状态 `DifyPersonVerifyState`（含 `reported_ids` set、`miss_frames` map、`last_upload_ms`、`first`）在 `logic_tools.h`。

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_dify_person_verify.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_dify_person_verify", logic_dify_person_verify);`。
- logics.json：`{ "name": "logic_dify_person_verify", "label": "人员 Dify 去重核验", "report": "dify", "params": [] }`

## 复用提示
**"每个目标只报一次、别重复刷"**的标准范式——核心就是用 `track_id` + `reported_ids` 集合 + `miss_frames` 容忍。做"每辆闯入的车上报一次""每个新进入的人记录一次"都套这套去重。比单纯按时间限频精准得多。
