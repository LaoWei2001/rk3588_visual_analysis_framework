# logic_person_alarm — 人员区域告警

- **上报**：无（只在画面显示 ALARM/CLEAR，状态存 `ctx->state` 供其它逻辑读取）
- **可调参数**：无
- **用到的能力**：ROI 内人员判定、`pointPolygonTest`、ROI 边界染色

## 做什么
ROI 内（无 ROI 则全屏）只要有 `person`，左上显示红色 `ALARM: N person(s) detected`、命中框标红、ROI 边界变红；否则显示绿色 `CLEAR`。把 `person_detected/person_count` 记在 `PersonAlarmState`，可被全局逻辑或上报模块读取来决定是否上报。**它本身不上报**——是"判定 + 可视化 + 暴露状态"的纯逻辑。

> 跨帧状态 `PersonAlarmState` 定义在 `logic_tools.h`。

## 完整实现
```cpp
static void logic_person_alarm(ChannelContext *ctx)
{
    if (!ctx) return;
    if (!*ctx->state) *ctx->state = std::make_shared<PersonAlarmState>();
    auto &s = *std::static_pointer_cast<PersonAlarmState>(*ctx->state);

    s.person_detected = false;
    s.person_count = 0;

    int has_roi = (ctx->roi && ctx->roi->size() >= 3);
    if (ctx->results)
        for (auto &r : *ctx->results) {
            if (r.label != "person") continue;
            if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0) continue;
            s.person_detected = true;
            s.person_count++;
            r.box_color = cv::Scalar(0,0,255);
            char label[64]; snprintf(label, sizeof(label), "person %.2f", r.score);
            draw_text(ctx, label, cv::Point(r.box.x, std::max(20, r.box.y - 8)), cv::Scalar(0,0,255), 0.5, 1);
        }

    if (s.person_detected) {
        char alarm_text[128];
        snprintf(alarm_text, sizeof(alarm_text), "ALARM: %d person(s) detected", s.person_count);
        draw_text(ctx, alarm_text, cv::Point(20,30), cv::Scalar(0,0,255), 0.7, 2);
    } else {
        draw_text(ctx, "CLEAR", cv::Point(20,30), cv::Scalar(0,255,0), 0.7, 2);
    }

    if (has_roi)
        for (size_t i = 0; i < ctx->roi->size(); ++i)
            draw_line(ctx, (*ctx->roi)[i], (*ctx->roi)[(i+1)%ctx->roi->size()],
                      s.person_detected ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0), 2);
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_person_alarm.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_person_alarm", logic_person_alarm);`。
- logics.json：`{ "name": "logic_person_alarm", "label": "人员区域告警", "params": [] }`

## 复用提示
"区域内有/无某类目标"的最干净判定 + 可视化模板。要加上报，在 `s.person_detected` 为真的分支里加限频 + `alarm_uploader_enqueue`（参考 `logic_server`/`logic_hook`），并在 logics.json 加 `"report": "server"`。
