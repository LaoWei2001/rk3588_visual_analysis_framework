# logic_roll — 传送带/料斗占用率

- **上报**：无（只在画面显示告警）
- **可调参数**：无（HSV 规则与阈值在代码里写死）
- **用到的能力**：ROI 必需、传统图像处理（HSV 占用率）、轮廓框、按 ROI 内外染色

## 做什么
在 ROI 内用 HSV 规则估算"非背景物料"的占用比例（`logic_roll_compute_occupancy` 计算），超过阈值 `0.10`（10%）就判 ALARM。把占用区域用轮廓框标红，ROI 边界按告警与否染红/绿，左上显示占用率/阈值/背景色相。检测框按是否在 ROI 内染黄/灰。

> 占用率计算的辅助函数 `logic_roll_compute_occupancy(...)` 在 `rk3588_yolo/src/logic/logic_tools.*`。这是"逻辑里也能用纯 OpenCV 传统视觉、不只靠 YOLO 结果"的例子。

## 完整实现
```cpp
static void logic_roll(ChannelContext *ctx)
{
    if (!ctx) return;
    if (!ctx->roi || ctx->roi->size() < 3) {
        draw_text(ctx, "roll: roi invalid", cv::Point(20,30), cv::Scalar(0,0,255), 0.6, 2); return;
    }
    if (!ctx->frame || ctx->frame->empty()) {
        draw_text(ctx, "roll: frame empty", cv::Point(20,30), cv::Scalar(0,0,255), 0.6, 2); return;
    }

    const int sat_min = 40, val_min = 40, hue_tol = 12, min_area = 100;
    const double threshold = 0.10;

    double ratio = 0.0; cv::Mat occupancy_mask; int bg_hue = 0;
    if (!logic_roll_compute_occupancy(*ctx->frame, *ctx->roi, sat_min, val_min, hue_tol, min_area,
                                      ratio, occupancy_mask, bg_hue)) {
        draw_text(ctx, "roll: occupancy compute failed", cv::Point(20,30), cv::Scalar(0,0,255), 0.6, 2); return;
    }

    int alarm = (ratio > threshold);
    const cv::Scalar roi_color = alarm ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0);
    for (size_t i = 0; i < ctx->roi->size(); ++i)
        draw_line(ctx, (*ctx->roi)[i], (*ctx->roi)[(i+1)%ctx->roi->size()], roi_color, 2);

    if (ctx->results)
        for (auto &r : *ctx->results) {
            int in_roi = (cv::pointPolygonTest(*ctx->roi, r.box_center(), false) >= 0.0);
            r.box_color = in_roi ? cv::Scalar(0,255,255) : cv::Scalar(120,120,120);
        }

    if (!occupancy_mask.empty()) {                       // 占用区域画轮廓框
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(occupancy_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (const auto &c : contours) {
            if (cv::contourArea(c) < min_area) continue;
            draw_rect(ctx, cv::boundingRect(c), cv::Scalar(0,0,255), 2);
        }
    }

    char info1[180];
    snprintf(info1, sizeof(info1), "Roll occupancy: %.2f%% (th=%.2f%%) bg_hue=%d %s",
             ratio*100.0, threshold*100.0, bg_hue, alarm ? "ALARM" : "OK");
    draw_text(ctx, info1, cv::Point(20,30), alarm ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0), 0.62, 2);

    char info2[180];
    snprintf(info2, sizeof(info2), "HSV rule: S>=%d V>=%d hue_tol=%d min_area=%d",
             sat_min, val_min, hue_tol, min_area);
    draw_text(ctx, info2, cv::Point(20,56), cv::Scalar(255,255,255), 0.5, 1);
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_roll.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_roll", logic_roll);`。
- logics.json：`{ "name": "logic_roll", "label": "传送带/料斗占用率", "params": [] }`
- 必须给该通道画 ROI（圈住传送带/料斗区域），否则显示 `roi invalid`。

## 复用提示
当 YOLO 检不出你要的东西（散料、积水、堆积），可以在 ROI 内用 HSV/边缘/帧差等传统手段判定——这个 logic 是范本：拿到 `*ctx->frame` 和 `*ctx->roi`，自己跑 OpenCV，再 `draw_*` 输出。
