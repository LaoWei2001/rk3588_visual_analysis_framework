# logic_cross_camera — 跨摄像头联动示例

- **上报**：无
- **可调参数**：无
- **用到的能力**：**跨通道取数**（`app_ctrl_get_channel_snapshot` / `get_channel_snapshot`）

## 做什么
在本通道里读取**另一个通道**的同帧画面 + 检测结果（原子快照，保证画面与框来自同一时刻），统计目标通道的人/车数量，左上打印目标通道信息。这是"一个通道的逻辑依赖另一个通道状态"（多路联动）的范本。

> 跨通道**只能**走快照接口拿数据，不要直接摸别的通道的内部状态（会有竞态）。`ChannelSnapshot` 含 `frame/results/disp_fps/infer_fps/frame_seq/result_age_ms`。

## 完整实现
```cpp
static void logic_cross_camera(ChannelContext *ctx)
{
    if (!ctx) return;
    int total_chns = app_ctrl_get_chn_nums();
    if (total_chns < 2) {
        draw_text(ctx, "Cross-Cam: Need >= 2 channels configured", cv::Point(20,30), cv::Scalar(0,0,255), 0.6, 2);
        return;
    }
    int target_chn = (ctx->chnId == 0) ? 1 : 0;
    if (target_chn >= total_chns) target_chn = 0;

    ChannelSnapshot snap;
    if (!app_ctrl_get_channel_snapshot(target_chn, &snap) || snap.frame.empty()) {
        draw_text(ctx, "Cross-Cam: snapshot unavailable", cv::Point(20,30), cv::Scalar(0,0,255), 0.6, 2);
        return;
    }
    std::string logic_name = app_ctrl_get_logic_name(target_chn);

    int person_cnt = 0, has_car = 0;
    for (const auto &r : snap.results) {
        if (r.label == "person") ++person_cnt;
        if (r.label == "car")    has_car = 1;
    }

    char info1[128], info2[128], info3[128];
    snprintf(info1, sizeof(info1), "Target CH: %d | Logic: %s", target_chn, logic_name.c_str());
    snprintf(info2, sizeof(info2), "Target FPS: D=%.1f I=%.1f | seq=%lld age=%lldms",
             snap.disp_fps, snap.infer_fps, (long long)snap.frame_seq, (long long)snap.result_age_ms);
    snprintf(info3, sizeof(info3), "Target Obj: Person=%d Car=%s Total=%zu",
             person_cnt, has_car ? "YES" : "NO", snap.results.size());

    int base_y = 30;
    draw_text(ctx, "[Cross Camera Demo]", cv::Point(20, base_y), cv::Scalar(0,255,255), 0.6, 2);
    draw_text(ctx, info1, cv::Point(20, base_y+30), cv::Scalar(255,255,255), 0.5, 1);
    draw_text(ctx, info2, cv::Point(20, base_y+60), cv::Scalar(255,255,255), 0.5, 1);
    draw_text(ctx, info3, cv::Point(20, base_y+90), cv::Scalar(255,255,255), 0.5, 1);
}
```

## 接线
- 注册：`register_logic("logic_cross_camera", logic_cross_camera);`
- logics.json：`{ "name": "logic_cross_camera", "label": "跨摄像头联动示例", "params": [] }`
- 需要 ≥2 个通道。

## 复用提示
做"A 路检到人且 B 路检到车才报警"这类多路联动，用 `ctx->get_channel_snapshot(目标通道)` 拿对方快照，记得用 `result_age_ms` 做新鲜度自检（太旧的快照别信）。
