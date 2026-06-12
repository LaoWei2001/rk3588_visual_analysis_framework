# logic_custom — 自定义示例（ROI 内外计数）

- **上报**：无
- **可调参数**：无
- **用到的能力**：ROI 判定、`pointPolygonTest`、逐目标染色/标注、管线信息显示

## 做什么
教学示例：画出 ROI 边界与顶点；对每个检测目标判定中心是否在 ROI 内，IN 染绿 / OUT 染红，并在框上标 `label score IN_ROI/OUT_ROI`；左上显示 ROI 内外计数；底部显示帧号/dt/推理开关。是"怎么用 `ctx` 各字段"的活文档。

## 完整实现
```cpp
static void logic_custom(ChannelContext *ctx)
{
    if (!ctx) return;
    int has_roi = (ctx->roi && ctx->roi->size() >= 3);

    if (has_roi) {
        for (size_t i = 0; i < ctx->roi->size(); ++i) {
            draw_line(ctx, (*ctx->roi)[i], (*ctx->roi)[(i+1)%ctx->roi->size()], cv::Scalar(0,255,255), 2);
            draw_circle(ctx, (*ctx->roi)[i], 3, cv::Scalar(0,255,255), 2);
        }
        cv::Point roi_txt((*ctx->roi).front().x + 4, std::max(20, (*ctx->roi).front().y - 8));
        draw_text(ctx, "ROI (scaled)", roi_txt, cv::Scalar(0,255,255), 0.55, 1);
    } else {
        draw_text(ctx, "ROI empty (pass-through)", cv::Point(20,30), cv::Scalar(0,165,255), 0.6, 2);
    }

    int in_cnt = 0, out_cnt = 0;
    if (ctx->results)
        for (auto &r : *ctx->results) {
            const cv::Point box_c = r.box_center();
            int in_roi = !has_roi || (cv::pointPolygonTest(*ctx->roi, box_c, false) >= 0.0);
            in_roi ? ++in_cnt : ++out_cnt;
            r.box_color = in_roi ? cv::Scalar(0,255,0) : cv::Scalar(0,0,255);
            draw_circle(ctx, box_c, 4, r.box_color, 2);
            char info[160];
            snprintf(info, sizeof(info), "%s %.2f %s", r.label.c_str(), r.score, in_roi ? "IN_ROI" : "OUT_ROI");
            draw_text(ctx, info, cv::Point(r.box.x, std::max(20, r.box.y - 8)), cv::Scalar(255,255,255), 0.5, 1);
        }

    if (has_roi) {
        char summary[96]; snprintf(summary, sizeof(summary), "ROI in:%d out:%d", in_cnt, out_cnt);
        draw_text(ctx, summary, cv::Point(20,30), cv::Scalar(0,255,255), 0.6, 2);
    }
    char pipe_info[128];
    snprintf(pipe_info, sizeof(pipe_info), "Frame ID: %lld | dt: %.1fms | Infer: %s",
             (long long)ctx->frame_id, ctx->dt_ms, ctx->infer_enabled ? "ON" : "OFF");
    draw_text(ctx, pipe_info, cv::Point(20,60), cv::Scalar(255,128,0), 0.6, 2);
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_custom.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_custom", logic_custom);`。
- logics.json：`{ "name": "logic_custom", "label": "自定义示例（ROI 计数）", "params": [] }`

## 复用提示
想快速验证"ROI 判定/染色/标注/计数"这些基本动作怎么写，照抄这里的片段。做"区域内 X 计数"的需求（`logic_count_in_roi` 形态）就是它的精简版。
