# logic_multi_roi — 多 ROI 区域示例

- **上报**：无
- **可调参数**：无
- **用到的能力**：多 ROI 访问（`ctx->rois` / `roi_count` / `roi_name_at` / `roi_polygon_at` / `roi_by_name` / `roi_count_target`(传序号或名字) / `roi_index_of`）、逐区域染色

## 做什么
演示"一个视频流配置多个 ROI 区域"后，逻辑里怎么方便地访问本通道的各个区域：
- 逐区域用不同颜色画出多边形，并在首个顶点旁标注 `[序号] 名字 person=N`（N=该区域内的人数）；
- 每个检测框按"中心落在哪个区域"染成该区域颜色，不在任何区域则染灰；
- 顶部显示区域总数；若存在名为 `entrance` 的区域，单独显示其人数（演示按名字访问）。

纯可视化，不报警不上报，无可调参数，无跨帧状态。坐标系全部是模型输入尺寸(640)。

## 完整实现
```cpp
static void logic_multi_roi(ChannelContext *ctx)
{
    if (!ctx) return;
    static const cv::Scalar kPalette[] = {
        cv::Scalar(0,255,255), cv::Scalar(0,255,0), cv::Scalar(255,128,0),
        cv::Scalar(255,0,255), cv::Scalar(0,165,255), cv::Scalar(255,255,0),
    };
    const int kNPal = (int)(sizeof(kPalette)/sizeof(kPalette[0]));

    const int nroi = ctx->roi_count();
    if (nroi == 0) {
        draw_text(ctx, "logic_multi_roi: no ROI (draw zones in web console)",
                  cv::Point(20,30), cv::Scalar(0,165,255), 0.6, 2);
        return;
    }

    // 1) 逐区域: 画闭合多边形 + 标注 [序号] 名字 person=N
    for (int i = 0; i < nroi; ++i) {
        const std::vector<cv::Point> *poly = ctx->roi_polygon_at(i);
        if (!poly || poly->size() < 3) continue;
        const cv::Scalar col = kPalette[i % kNPal];
        draw_polyline(ctx, *poly, col, 2, 1.0, /*closed=*/true);
        const char *nm = ctx->roi_name_at(i);
        const int persons = roi_count_target(ctx, "person", i);
        char label[96];
        if (nm && nm[0]) snprintf(label, sizeof(label), "[%d] %s  person=%d", i, nm, persons);
        else             snprintf(label, sizeof(label), "[%d] zone  person=%d", i, persons);
        draw_text(ctx, label, cv::Point(poly->front().x + 4, std::max(16, poly->front().y - 6)), col, 0.5, 1);
    }

    // 2) 逐检测框: 落在哪个区域就染成该区域色, 否则灰
    if (ctx->results)
        for (auto &r : *ctx->results) {
            const int hit = ctx->roi_index_of(r.box);   // 落在第几个区域, 都不在 → -1
            const cv::Scalar col = (hit >= 0) ? kPalette[hit % kNPal] : cv::Scalar(160,160,160);
            r.box_color = col;
            draw_circle(ctx, r.box_center(), 3, col, 2);
        }

    // 3) 顶部汇总 + 演示按名字访问
    char summary[64]; snprintf(summary, sizeof(summary), "ROI zones: %d", nroi);
    draw_text(ctx, summary, cv::Point(20,24), cv::Scalar(255,255,255), 0.6, 2);
    if (ctx->roi_by_name("entrance")) {
        const int n = roi_count_target(ctx, "person", roi_find(ctx, "entrance"));
        char t[64]; snprintf(t, sizeof(t), "entrance person=%d", n);
        draw_text(ctx, t, cv::Point(20,48), cv::Scalar(0,255,0), 0.55, 2);
    }
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_multi_roi.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_multi_roi", logic_multi_roi);`。
- logics.json：`{ "name": "logic_multi_roi", "label": "多 ROI 区域示例", "params": [] }`
- 网页：把该通道「逻辑函数」节点选 `logic_multi_roi`；在「ROI 检测区域」节点点「绘制 ROI 区域」，在弹窗右侧「区域列表」里「＋新增区域」画多个区域并各自命名（如 `entrance`/`exit`）。一个 ROI 节点即可承载该通道的全部区域。

## 多 ROI API（`ChannelContext` 成员 + `channel_logic.h` 顶层自由函数）
| 方法 | 含义 |
|------|------|
| `ctx->rois` | `const std::vector<RoiZone>*`，本通道全部区域（`RoiZone{ name, polygon }`，polygon 为模型坐标） |
| `ctx->roi`  | 兼容字段，指向第一个区域的多边形（无区域=nullptr） |
| `ctx->roi_count()` | 区域数量 |
| `ctx->roi_at(i)` / `roi_polygon_at(i)` / `roi_name_at(i)` | 按序号取 区域 / 多边形 / 名字 |
| `ctx->roi_by_name("entrance")` | 按名字取区域 |
| `roi_contains(ctx, box, idx)` | 框中心是否在 ROI 内（`idx=ROI_ALL` 任一区域；`>=0` 第 i 区） |
| `roi_count_target(ctx, "person", i)` | 第 i 区域内某类别数量 |
| `roi_count_target(ctx, "person", roi_find(ctx, "entrance"))` | 按名字统计 |

## 复用提示
做"分别统计入口/出口人数""目标从 A 区进入 B 区""不同区域不同阈值/报警"这类需求时，
照这里的范式：先 `ctx->roi_by_name(...)` 或 `ctx->roi_polygon_at(i)` 拿到区域，再 `pointPolygonTest` 判定。
跨帧计时/报警仍走 `ctx->state`，上报仍走 `*_uploader_enqueue` + `ctx->config->server_url`（见 logic_hook / logic_server）。
