# Rockchip 平台兼容性基线

`PLATFORM_COMPATIBILITY.env` 记录项目已经在真实 RK3588 上验证过的系统、内核、
RKNPU、RGA 和 Rockchip MPP GStreamer 插件组合。`install_deps.sh --check` 会将目标
设备的实际环境与该文件比较。

检测到不同版本时只代表“未经本项目验证”，不能单凭组件来自瑞芯微官方就断定兼容或
不兼容。必须至少完成以下现场冒烟测试后，才能更新基线：

1. 使用实际 `.rknn` 模型完成初始化和持续推理；
2. 通过 RGA 完成项目使用的颜色转换、缩放和 DMA-BUF 零拷贝；
3. 使用 `mppvideodec` 持续解码实际 RTSP 流，并验证断流重连；
4. 使用 `mpph264enc`/`mpph265enc` 验证 RTSP 输出或事件录像；
5. 验证 USB/MIPI 摄像头、GPIO/继电器等现场实际启用的设备。

这个基线不替代瑞芯微的正式兼容矩阵，也不表示其他版本一定不能运行。
