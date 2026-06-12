# logic_roi — ROI 顶点坐标 + 检测框中心坐标可视化（中心入区染红）

**需求**：在画面上把 ROI 每个顶点的坐标标在该顶点旁；每个检测框中心标注中心坐标；任一目标中心落在 ROI 内，就把该目标的检测框染成红色。

**性质**：纯可视化/调试逻辑——不报警、不上报、无可调参数、无跨帧状态（不需要 `ctx->state`）。坐标系全部是**模型输入尺寸(640)**，与 `ctx->roi` / `box` 一致，直接画即可。

## 完整代码（`rk3588_yolo/src/logic/logic_roi.cpp`）

```cpp
/* logic_roi —— ROI 顶点坐标 + 检测框中心坐标可视化；目标中心落在 ROI 内则该框染红。 */
static void logic_roi(ChannelContext *ctx)
{
    if (!ctx) return;

    const int has_roi = (ctx->roi && ctx->roi->size() >= 3);

    /* 1) ROI 各顶点：画边 + 顶点标记 + 在旁边标注该点坐标 */
    if (has_roi)
    {
        const size_t n = ctx->roi->size();
        for (size_t i = 0; i < n; ++i)
        {
            const cv::Point &p  = (*ctx->roi)[i];
            const cv::Point &pn = (*ctx->roi)[(i + 1) % n];
            draw_line(ctx, p, pn, cv::Scalar(0, 255, 255), 1);   /* ROI 边（黄、细） */
            draw_circle(ctx, p, 4, cv::Scalar(0, 255, 255), 2);  /* 顶点标记 */
            char vtxt[48];
            snprintf(vtxt, sizeof(vtxt), "(%d,%d)", p.x, p.y);
            draw_text(ctx, vtxt, cv::Point(p.x + 6, std::max(12, p.y - 6)),
                      cv::Scalar(0, 255, 255), 0.45, 1);         /* 顶点坐标（黄） */
        }
    }
    else
    {
        draw_text(ctx, "logic_roi: no ROI", cv::Point(20, 30),
                  cv::Scalar(0, 165, 255), 0.6, 2);
    }

    /* 2) 每个检测框：中心标注坐标；中心落在 ROI 内 → 该框染红 */
    if (ctx->results)
    {
        for (auto &r : *ctx->results)
        {
            const cv::Point c = r.box_center();
            const bool inside = has_roi &&
                                cv::pointPolygonTest(*ctx->roi, c, false) >= 0.0;

            if (inside)
                r.box_color = cv::Scalar(0, 0, 255);             /* 红：中心在区域内 */

            const cv::Scalar mark = inside ? cv::Scalar(0, 0, 255)   /* 红 */
                                           : cv::Scalar(0, 255, 0);  /* 绿 */
            draw_circle(ctx, c, 3, mark, 2);                     /* 中心点标记 */
            char ctxt[48];
            snprintf(ctxt, sizeof(ctxt), "(%d,%d)", c.x, c.y);
            draw_text(ctx, ctxt, cv::Point(c.x + 6, c.y + 4), mark, 0.45, 1);
        }
    }
}
```

## 接线

1. **文件 / 注册**：独立文件 `src/logic/logic_roi.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_roi", logic_roi);`（无需改动 `channel_logic.cpp`）。
2. **logics.json**（`channel_logics` 数组，无上报所以不写 `report`）：
   ```json
   { "name": "logic_roi", "label": "ROI/坐标可视化", "params": [] }
   ```
3. 无可调参数 → 不动 `config.h` / `config_init.cpp`。

## 复用提示 / 范式要点

- **染框靠 `r.box_color`，不用自己画框**：框由显示渲染层按 `results` 画，置 `r.box_color = (0,0,255)` 即变红（`box_color[0] >= 0` 才生效）。这与 `logic_custom` / `logic_person_alarm` 一致。
- **"中心在区域内" 判定**：`cv::pointPolygonTest(*ctx->roi, r.box_center(), false) >= 0`。**先判 `has_roi`**——无 ROI 时不存在"区域内"，不应染红（不要用 `ctx->is_in_roi()`，它在无 ROI 时返回 1）。
- **坐标系**：`ctx->roi` 与 `box` 都是模型 640 空间，文字/点直接用这套坐标画，和画面对齐；别拿源分辨率换算。
- **画文字/点用 `draw_*`**：它们把指令塞进 `ctx->draw_cmds`，由渲染层统一绘制；`target` 默认 `ALL`（显示+上报图都画），本逻辑不上报，用默认即可。
- 想区分"框上方标签"和"中心坐标"：本逻辑把坐标画在**中心点**(`c.x+6, c.y+4`)，顶点坐标画在**顶点旁**(`p.x+6, p.y-6`)，互不挤占。
