# 性能实测与复现

报告仅适用于各自记录的源码、平台、模型、输入和采样条件；本轮整理没有重跑硬件性能测试。

[返回文档总入口](../README.md)

| 文档 | 内容 |
| --- | --- |
| [基线与验证入口](benchmark.md) | 测量口径、软件回归命令和历史验收范围 |
| [NPU 核心分配](npu-core-placement.md) | 手动/自动分核的逐路比较 |
| [实时多路推理](npu-live-performance.md) | 真实输入与原生输出实验 |
| [多模型扩展实验](npu-live-performance-extended.md) | 同帧多模型、混合路数与 worker 对照 |
| [ZipDepth 性能](zip-depth-performance.md) | 深度模型条件、数值与吞吐 |
| [性能叠加绘制](performance-display-benchmark.md) | 文字刷新与显示耗时 |
| [Web 播放延迟](web-live-latency.md) | 缓冲、重连与历史修复验证 |
| [多出口延迟对照](live-low-latency.md) | HDMI、RTSP、Web 的实验条件与限制 |

原始测量 JSON 在 [data/](data/)。其路径、哈希和时间保留原始值；源码搬迁不改写历史证据。
