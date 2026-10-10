# 区域持续占用：SDK 实战示例

源码 [logic.cpp](logic.cpp) 只依赖 `channel.h`、`drawing.h`、`events.h` 和标准库。
参数、输出和事件字段全部在 [logic.json](logic.json) 中声明。计时、报警和重试策略
属于本模块；没有把具体业务函数加入 `engine/include/`。
目标筛选使用 `TargetQuery` 和 `ctx->query_targets()`；查询统一处理 ROI 和推理有效性，
具体规则见 [通用目标查询](../../../docs/sdk/target-query.md)。

## 在业务中使用

1. 重新构建后，在通道选择 `logic_roi_dwell_demo`（SDK 示例：区域持续占用报警）。
2. 开启推理，选择合适的模型，把 `target_label` 改成模型实际输出的标签。
3. 绘制一个多边形 ROI，命名为 `alarm_zone`，或修改参数 `roi_name` 与区域名称一致。
4. 设置 `hold_ms`，默认 2000 毫秒；`max_gap_ms` 默认 1000 毫秒，应大于正常业务帧间隔。
5. 需要保存或发送事件时，在上报节点配置 `roi_dwell` 对应的投递策略。

```json
{
  "target_label": "person",
  "roi_name": "alarm_zone",
  "hold_ms": 2000,
  "max_gap_ms": 1000
}
```

这是 `logic_parameters` 对象示例，不是完整通道配置。

## 行为约定

- 按目标框中心是否落在命名区域内计数，包含边界；区域不存在或不足三个顶点时停止计时。
- “持续出现”指区域持续有该类别的目标，不要求同一个跟踪 ID。任一有效帧没有目标就清零；
  本示例没有漏检宽限期。推理无效、未绑定结果、关闭推理也清零，不沿用旧检测结果。
- 使用 `timestamp_ms` 计时。帧间隔超过 `max_gap_ms` 或时间倒退时重新计时，避免断流误报。
- 持续时间达到阈值后，区域变红并提交事件；`hold_ms=0` 表示首次出现立即触发。
- 同一次连续占用只接受一个事件；提交失败最多每秒重试一次，目标消失则取消重试。
  `CREATED_MEDIA_FAILED` 仍算已接受，避免因媒体失败重复创建事件。
- 四个参数均声明 `reset_state`，修改后从新条件重新计时；框架修改 ROI 几何时也重置逻辑状态。
- `alarm_active` 表示检测条件满足；`event_accepted` 表示本次事件被本地事件入口接受，
  **不表示远端已经收到**。查看事件列表才能确认媒体、持久化和投递的后续结果。
- 先 `draw_*()` 再 `report_event()`，让事件入口能取到当前报警绘图。

每帧发布 `target_count`、`dwell_ms`、`alarm_active`、`event_accepted` 和 `status`，
可连接到全局模块使用。`status` 为 `roi_missing`、`inference_unavailable`、`waiting`、
`timing` 或 `alarm`。

## 可重复验收

在仓库根目录执行：

```bash
cmake -S engine -B build/engine -DBUILD_SDK_TESTS=ON
cmake --build build/engine --parallel 4
(cd build/engine && ctest --output-on-failure -R '^sdk_')
```

运行测试时，业务模块仍然单独以公开 SDK 路径编译。测试夹具负责绑定内部上下文，参数解析、
注册、ROI、输出和绘图调用真实引擎代码；事件出口用可控接收器记录请求、模拟接受与失败，
不启动事件存储、上传服务或 GPIO。Web 参数修改验证覆盖 Schema 解析、重载策略和按策略
重建状态，不包含浏览器操作或配置监视线程。

可选录像回放，使用已有录像画面与可控检测序列验证绘图和报警时序：

```bash
build/engine/sdk_business_tests build/engine/sdk-acceptance projects/assets/10_9_2.mp4
```

回放前 8 秒，保留源帧率。输入必须至少有 8 秒视频。输出目录由上述 CMake 配置创建。
产物为 `build/engine/sdk-acceptance/alarm.png`、`replay_alarm.png`、`replay.avi` 和 `replay.csv`。
画面明确标注 `SCRIPTED DETECTIONS`：检测框是确定的测试输入，这项回放不评估 NPU 识别精度。
