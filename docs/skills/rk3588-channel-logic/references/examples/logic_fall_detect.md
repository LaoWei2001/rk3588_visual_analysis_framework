# logic_fall_detect — 人员跌倒 / 挥手求救检测

- **上报**：server（`alarm_uploader_enqueue`，类型 `"fall_detect"` 与 `"wave_sos"`，各自限频）
- **可调参数**：`fall_ratio_thresh`(float)、`fall_dwell_sec`(float)、`fall_cooldown_sec`(int)、`wave_min_swings`(int)、`wave_window_sec`(float)
- **用到的能力**：pose 关键点（yolov8_pose，无关键点时退回检测框宽高比）、dwell 持续确认 + cooldown 限频 + 闩锁、ROI 过滤（`roi_contains`）、挥手摆动状态机

## 做什么
对 ROI 内（无 ROI 则全屏）的每个 `person`：

1. **跌倒**：优先用 pose 的肩/髋关键点判断躯干是否接近横躺（`fall_pose_like`）；若模型无关键点，退回**检测框宽高比** `width/height >= fall_ratio_thresh`。任一成立即"跌倒嫌疑"，框标红 + `fall?`。嫌疑**持续 ≥ `fall_dwell_sec` 秒**才真正报警（过滤弯腰/短暂误检），并按 `fall_cooldown_sec` 限频上报 `"fall_detect"`，闩锁防重复。
2. **挥手求救**：检测手腕高于肩部（`wave_pose_candidate`）后，在 `wave_window_sec` 窗口内统计手腕左右方向切换次数，达到 `wave_min_swings` 即判挥手求救，上报 `"wave_sos"`（独立 cooldown + 闩锁）。窗口超时未达标则重新计数。

> 跨帧状态 `FallDetectState`（fall/wave 各自的 `start_ms`/`last_upload_ms`/闩锁 + 挥手的 `last_wrist_x`/`wave_dir`/`wave_swings`）定义在 `logic_fall_detect.cpp` 内（文件私有）。

## 实现要点（完整代码见 `src/logic/logic_fall_detect.cpp`）
两个 pose 辅助函数（都要求 `r.keypoints.size() >= 17`，COCO 17 点）：
```cpp
// 躯干接近横躺 或 框横置 → 跌倒嫌疑
static bool fall_pose_like(const AlgoResult &r, float ratio_thresh);
//   肩(5,6)/髋(11,12) 中点的 |dx| > |dy|*1.25  ||  box.width/box.height >= ratio_thresh

// 手腕(9/10)高于肩(5/6)且肘(7/8)合理 → 候选挥手，输出手腕坐标
static bool wave_pose_candidate(const AlgoResult &r, cv::Point2f *wrist_out);
```
主函数流程（条件分支与限频已精简注释）：
```cpp
static void logic_fall_detect(ChannelContext *ctx)
{
    if (!ctx || !ctx->results) return;
    if (!*ctx->state) *ctx->state = std::make_shared<FallDetectState>();
    auto &s = *std::static_pointer_cast<FallDetectState>(*ctx->state);

    // 1) 参数每帧现读 + clamp（热重载）
    float ratio_thresh   = ctx->config ? ctx->config->fall_ratio_thresh : 1.25f;
    float dwell_sec      = ctx->config ? ctx->config->fall_dwell_sec     : 2.0f;
    int   cooldown_sec   = ctx->config ? ctx->config->fall_cooldown_sec  : 10;
    int   wave_min_swings= ctx->config ? ctx->config->wave_min_swings    : 2;
    float wave_window_sec= ctx->config ? ctx->config->wave_window_sec    : 2.0f;
    // ... 各自下限保护 ...

    // 2) 遍历 person（ROI 过滤），判跌倒嫌疑 / 挥手候选，命中染色+标注
    bool fall_like=false, hand_raised=false; int person_count=0;
    cv::Rect best_box; cv::Point2f wave_wrist; float best_score=-1;
    for (auto &r : *ctx->results) {
        if (r.label != "person" || !roi_contains(ctx, r.box, ROI_ALL)) continue;
        ++person_count;
        float box_ratio = r.box.height>0 ? (float)r.box.width/r.box.height : 0;
        if (fall_pose_like(r, ratio_thresh) || box_ratio >= ratio_thresh) { /* 框红+"fall?"，记 best_box */ fall_like=true; }
        cv::Point2f wrist;
        if (wave_pose_candidate(r, &wrist)) { hand_raised=true; wave_wrist=wrist; /* 画手腕+"wave?" */ }
    }

    // 3) 挥手摆动状态机（窗口内左右切换计数 → wave_alarm）
    bool wave_alarm = false;
    if (hand_raised) { /* 用 wave_start_ms 维护窗口；dx 超阈值判方向，方向反转 ++wave_swings；超 wave_min_swings → wave_alarm */ }
    else { /* 复位挥手状态 */ }

    // 4) 跌倒 dwell：持续 ≥ dwell_sec 才报警；cooldown + 闩锁限频上报 "fall_detect"
    if (fall_like) {
        if (s.fall_start_ms==0) s.fall_start_ms = ctx->timestamp_ms;
        float elapsed = (ctx->timestamp_ms - s.fall_start_ms)/1000.0f;
        if (elapsed >= dwell_sec) { /* 画 ALARM；若 !latched 且距上次 >= cooldown 且有帧 → alarm_uploader_enqueue(... "fall_detect", ctx->config->report_enable, url)，置 latched */ }
        else { /* 画 "Fall suspicious x/ys" */ }
    } else { s.fall_start_ms=0; s.alarm_latched=false; /* 画 "Fall: CLEAR person:N" */ }

    // 5) 挥手报警：cooldown + 闩锁上报 "wave_sos"（ctx->config->report_enable 作为参数传入）
    if (wave_alarm) { /* 画 "ALARM: WAVE SOS"；cooldown+latch → alarm_uploader_enqueue(... "wave_sos", ctx->config->report_enable, url) */ }
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_fall_detect.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_fall_detect", logic_fall_detect);`。
- logics.json：`{ "name": "logic_fall_detect", "label": "人员跌倒/挥手求救检测", "report": "server", "params": [ fall_ratio_thresh / fall_dwell_sec / fall_cooldown_sec / wave_min_swings / wave_window_sec ] }`（5 个参数详见 logics.json）。
- 5 个参数四处对齐（均热重载）：`config.h` 的 `ChannelConfig` 字段 + `config_init.cpp` 的 `REG_C` + logics.json 声明 + 逻辑里 `ctx->config->xxx` 现读。
- 模型建议用 **yolov8_pose**（有 17 关键点判躯干姿态最准）；普通检测模型也能跑，但只能靠框宽高比，易误判。
- 上报：网页给该通道连「上报配置」节点填 server 地址。

## 复用提示
- **pose 关键点 + 框比例双判**：有关键点用 `r.keypoints[5/6/11/12]` 判躯干姿态，无则退回 `box.width/box.height`——"姿态类"判定（跌倒/弯腰/举手）的通用骨架。
- **dwell + cooldown + 闩锁** 三件套：`fall_start_ms` 持续确认、`last_upload_ms` 冷却、`alarm_latched` 防重复——所有"持续 N 秒才报、报后限频"的需求都套这套（同 `logic_hook`）。
- **窗口内计数**：挥手用"窗口期 + 方向切换计数 + 超时重置"识别周期动作，可迁移到"招手/摇头/往复擦拭"等。
