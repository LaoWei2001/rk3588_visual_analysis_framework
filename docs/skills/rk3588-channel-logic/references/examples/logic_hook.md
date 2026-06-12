# logic_hook — 吊钩安全圈检测

- **上报**：server（`alarm_uploader_enqueue`，类型 `"hook_tilt"`，带框上报图）
- **可调参数**：`radius`（安全圈半径，int）
- **用到的能力**：跨帧状态机 + 计时 + 闩锁 + 宽限期、`r.dist_sq_to`、按 target 分别绘制、带框上报图

## 做什么
画面中心 `(330,320)` 处有一个半径 `radius` 的"安全圈"。检测目标 `hook_l`：
- 钩子中心**出圈**持续 `0.8s` → 触发一次告警并上报（带框图），随后闩锁不再重复发；
- 钩子**回圈内**持续 `3s` → 复位闩锁，可再次告警；
- 检测短暂丢失（≤500ms）进入 `SEARCHING` 宽限期，不误判。
左上显示 OUT/IN 秒数和状态（SEARCHING/NO HOOK/READY/COOLING/ALARMED）。

> 跨帧状态结构 `HookState`（含 `alarm_active/alarm_sent_latch/alarm_reset_counting/disp_outside_sec/...`）定义在 `rk3588_yolo/src/logic/logic_tools.h`。

## 完整实现
```cpp
static void logic_hook(ChannelContext *ctx)
{
    if (!ctx) return;
    if (!*ctx->state) *ctx->state = std::make_shared<HookState>();
    auto &s = *std::static_pointer_cast<HookState>(*ctx->state);

    const cv::Point safe_center(330, 320);
    const int radius = ctx->config ? ctx->config->radius : 1;     // 可调参数
    const int radius_sq = radius * radius;
    const float alarm_duration_sec = 0.8f;   // 出圈多久算告警
    const float reset_duration_sec = 3.0f;   // 回圈多久算复位
    const float dt_sec = ctx->dt_ms / 1000.0f;
    const uint64_t no_detect_grace_ms = 500; // 丢检宽限

    int found_hook = 0, hook_outside = 0;
    if (ctx->results)
        for (AlgoResult &r : *ctx->results) {
            if ((r.class_id != 0) || (r.label != "hook_l")) continue;
            found_hook = 1;
            hook_outside = hook_outside || (r.dist_sq_to(safe_center) > radius_sq);
        }

    if (found_hook) s.last_found_ms = ctx->timestamp_ms;
    int in_grace = !found_hook && (ctx->timestamp_ms - s.last_found_ms <= no_detect_grace_ms);

    const cv::Scalar circle_color = (hook_outside || (in_grace && s.alarm_active))
                                        ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0);
    draw_circle(ctx, safe_center, radius, circle_color, 2, DrawCommand::ALL);

    /* 状态面板（只画到显示器） */
    {
        const int tx = 410, ty = 330, line_h = 45;
        char line1[64], line2[64];
        snprintf(line1, sizeof(line1), s.alarm_sent_latch ? "OUT: %.1fs [ALARM]" : "OUT: %.1fs", s.disp_outside_sec);
        draw_text(ctx, line1, cv::Point(tx, ty), cv::Scalar(0,0,255), 1, 2, DrawCommand::DISPLAY);
        snprintf(line2, sizeof(line2), s.alarm_sent_latch ? "IN:  %.1fs / %.0fs" : "IN:  %.1fs", s.disp_inside_sec, reset_duration_sec);
        draw_text(ctx, line2, cv::Point(tx, ty + line_h), cv::Scalar(0,255,0), 1, 2, DrawCommand::DISPLAY);

        const char *status_str; cv::Scalar status_color;
        if (in_grace)                    { status_str = "SEARCHING"; status_color = cv::Scalar(0,200,255); }
        else if (!found_hook)            { status_str = "NO HOOK";   status_color = cv::Scalar(180,180,180); }
        else if (!s.alarm_sent_latch)    { status_str = "READY";     status_color = cv::Scalar(0,255,0); }
        else if (s.alarm_reset_counting) { status_str = "COOLING";   status_color = cv::Scalar(0,165,255); }
        else                             { status_str = "ALARMED";   status_color = cv::Scalar(0,0,255); }
        draw_text(ctx, status_str, cv::Point(tx, ty + line_h*2), status_color, 1, 2, DrawCommand::DISPLAY);
    }

    if (in_grace) return;

    if (hook_outside) {
        s.alarm_reset_counting = false; s.disp_inside_sec = 0.0f;
        if (!s.alarm_active) { s.alarm_active = true; s.alarm_start_ms = ctx->timestamp_ms; }
        if (!s.alarm_sent_latch) s.disp_outside_sec += dt_sec;

        float outside_elapsed = (ctx->timestamp_ms - s.alarm_start_ms) / 1000.0f;
        if (!s.alarm_sent_latch && outside_elapsed >= alarm_duration_sec) {
            if (ctx->frame) {
                cv::Mat upload_img = ctx->frame->clone();
                RenderParams rp = ctx->render_params();
                rp.show_fps = 0;
                rp.target_mask = DrawCommand::UPLOAD;      // 把 UPLOAD 目标的绘制叠上去
                render_overlays(upload_img, rp);
                alarm_uploader_enqueue(upload_img, *ctx->frame, ctx->chnId, "hook_tilt",
                                       ctx->config ? ctx->config->server_url.c_str() : nullptr);
            }
            s.alarm_sent_latch = true;                     // 闩锁：只发一次
        }
    } else {
        s.alarm_active = false;
        if (!s.alarm_sent_latch) s.disp_outside_sec = 0.0f;
        if (s.alarm_sent_latch && found_hook) {            // 回圈内开始计复位时间
            s.disp_inside_sec += dt_sec;
            if (!s.alarm_reset_counting) { s.alarm_reset_counting = true; s.alarm_reset_start_ms = ctx->timestamp_ms; }
            else if ((ctx->timestamp_ms - s.alarm_reset_start_ms)/1000.0f >= reset_duration_sec) {
                s.alarm_sent_latch = false; s.alarm_reset_counting = false;
                s.disp_outside_sec = 0.0f; s.disp_inside_sec = 0.0f;
            }
        } else { s.alarm_reset_counting = false; s.disp_inside_sec = 0.0f; }
    }
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_hook.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_hook", logic_hook);`。
- logics.json：
  ```json
  { "name": "logic_hook", "label": "吊钩安全圈检测", "params": [
      { "key": "radius", "type": "int", "label": "安全圈半径", "default": 50, "min": 1, "max": 1000,
        "help": "目标中心离安全圈中心超过此半径即判定出圈告警" } ] }
  ```
  （注意：要在 logics.json 加 `"report": "server"` 才会提示连上报节点；该 logic 实际用了 server 上报。）

## 复用提示
这是"**持续 N 秒才告警、告警只发一次、条件消失后复位**"的标准范式（闩锁 + 双向计时 + 宽限期）。做"越界/离岗/停留"类需求直接套这套状态机，把 `hook_outside` 换成你的条件即可。
