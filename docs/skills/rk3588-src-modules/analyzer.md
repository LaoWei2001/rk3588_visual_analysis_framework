# `src/analyzer`：帧处理与推理调度总枢纽

## 文件地图

| 文件 | 当前职责 |
|---|---|
| `analyzer.cpp/.h` | 生命周期、线程入口、通道在线/健康状态、ROI 装载 |
| `frame_inlet.cpp` | `videoOutHandle()` 的逐帧入口、节流、录像预缓存、显示和推理分流 |
| `rga_convert.cpp` | 源格式到模型输入的 RGA/软件转换 |
| `algoProcess.cpp` | 通道模型/worker/队列生命周期和公共算法 API |
| `algo_engine.cpp` | 模型工厂、NPU worker、单/多模型执行和结果过滤 |
| `result_dispatch.cpp` | 等待并取出推理结果 |
| `channel_pipeline.cpp` | tracker、动作、通道 logic、同帧状态提交 |
| `display_pipeline.cpp` | 每通道单槽显示队列、最新画面与最近结果组合 |
| `display_render.cpp` | tile 计算和拼接缓冲提交 |
| `tracker.cpp` | SORT/卡尔曼跟踪与速度估计 |

## 单帧路径

appsink 调用 `videoOutHandle()` 后，frame inlet 首先更新源尺寸并把原始帧送入事件录像预缓存。随后检查 `infer_enable` 与运行时开关、按 `max_fps` 做相位错开的推理节流，将帧送显示单槽，并在需要推理时转换为模型输入后调用 `algorithm_process_mat()`。

未启用推理的通道不进 NPU，但会把空结果和模型输入帧交给通道 logic，因此传统 OpenCV logic 仍可逐帧工作。

每个推理通道的 dispatch worker 等待 NPU 结果，取出“结果 + 产生结果的模型输入帧 + frame id”。`channel_pipeline` 先执行 tracker，再构造 `ChannelContext`；它在逐帧 logic 之前消费 Web 动作，最后把匹配帧、结果、状态、绘制指令和可选 canvas 原子写回 `ChannelState`。

## 显示与业务一致性的差别

显示路径追求低延迟：每通道只有一个待显示槽位，新帧覆盖旧帧；渲染可使用最近一次结果，并用 tracker 速度对结果延迟做有限外推。业务路径追求确定性：logic、告警和 `ChannelSnapshot` 使用与结果相同的模型输入帧。二次开发不能把显示帧误当作同帧推理证据。

## `AlgoResult`

基础字段包括 box、label/class/score、track id、通道/帧/时间；多模型来源字段为 `model_id`、`model_type`、`model_index`；tracker 填 `vx/vy/track_hits`；姿态、OCR、分割分别使用 keypoints/keypoint_scores、text_result、boxMask。

## RGA 约束

`frame_pipeline.h` 明确要求 RGA 调度使用 `IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1`。项目注释记录切到 RGA2 或第三核心可能导致设备硬崩溃；没有板端验证依据时不要改这部分。DMA-BUF 必须在原 FD 有效时导入为 `RgaImportedBuffer`，由 RAII 保持内核引用，不能把 appsink 回调中的裸 FD 留给异步线程。

预处理是整幅 resize 到模型输入尺寸，不做 letterbox，所以 ROI、结果和 logic 坐标直接统一。

## 生命周期

`analyzer_init()` 初始化数据结构、算法、tracker、logic/global logic，但 display/dispatch pthread 由 `main()` 创建。`analyzer_deinit()` 停推理与业务子系统；main 随后 join dispatch/display，最后才销毁显示队列同步原语。

断流调用 `analyzer_channel_offline()` 清理节流/tracker 并通知 recorder；恢复调用 online 入口清旧结果和绘制，避免冻结框污染新流。
