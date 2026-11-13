# 性能实测与复现

按模型、并发路数和输出负载查看板端实测。各报告保留结果汇总、测试条件、统计口径和复验方法。

[返回文档总入口](../README.md)

| 文档 | 内容 |
| --- | --- |
| [四类模型多路并发](model-channel-fps.md) | 2026-11-13：1～15 路完整视频管线、48 组正式测量，含本地显示与硬件 RTSP |
| [基线与验证入口](benchmark.md) | 测量口径、软件回归命令和历史验收范围 |
| [NPU 核心分配](npu-core-placement.md) | 手动/自动分核的逐路比较 |
| [实时多路推理](npu-live-performance.md) | 真实输入与原生输出实验 |
| [多模型扩展实验](npu-live-performance-extended.md) | 同帧多模型、混合路数与 worker 对照 |
| [ZipDepth 性能](zip-depth-performance.md) | 深度模型条件、数值与吞吐 |
| [性能叠加绘制](performance-display-benchmark.md) | 文字刷新与显示耗时 |
| [Web 播放延迟](web-live-latency.md) | 缓冲、重连与历史修复验证 |
| [多出口延迟对照](live-low-latency.md) | HDMI、RTSP、Web 的实验条件与限制 |
