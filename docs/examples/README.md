# 配置示例

[返回文档总入口](../README.md)

[quickstart.json](quickstart.json) 是无 NPU 推理的文件输入示例，开启 RTSP 预览。
`assets/input.mp4` 是占位路径，需要提供真实可解码的视频；无推理不等于无需 RK3588 平台依赖。
构建与运行步骤见[快速开始](../../README.md#快速开始)。

`--validate-config` 只校验配置和资源路径，不启动媒体、显示或推理。
配置加载通过后仍需单独验证视频播放和板端硬件。
