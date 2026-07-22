# sop_agent

本项目已收敛为 RK3588 上的单一 SOP 路径合规检测应用。

核心业务逻辑：`rk3588_yolo/src/logic/logic_path_sop.cpp`

保留能力：

- RTSP、文件、USB 视频采集
- YOLOv5 / YOLOv8 目标检测，并保留姿态、分割模型实现
- 多 ROI 与 SOP 有向流程编排
- 进入确认、停留上下限、总耗时、分支、环路及循环次数检查
- 顺序错误、漏步骤、停留违规及耗时违规报警
- 通用告警事件发件箱：服务器图片、Dify 图片、Dify 视频
- 原始分辨率的报警前后事件视频（无音频）
- Web 控制台配置与热重载

默认配置：`rk3588_yolo/assets/config_sop.json`

## 构建

在 RK3588 板端或配置好交叉编译环境后执行：

```bash
cd rk3588_yolo
./build.sh
```

## 运行

```bash
./rk3588_yolo ./assets/config_sop.json
```

开发新功能时，以 `logic_path_sop.cpp`、`ChannelConfig` 中的 `path_*` 字段和 Web SOP 编排组件为唯一业务基线。

## 通用告警上报框架

业务逻辑统一调用 `alarm_report(ctx, request)`，只提交报警类型、说明和运行时字段。当前通道画布中的“上报配置”节点决定：

- 保留图片、视频或两者；
- 图片发往业务服务器或 Dify，视频发往 Dify；
- 每个投递使用哪个连接 Profile、Dify 文件变量和 JSON 参数映射；
- 视频报警前/后秒数、帧率，以及连续报警合并窗口。

同通道、同报警类型在 5 秒内合并。每个投递独立记录状态；全部投递成功后立即删除本地事件，失败或断网时继续保留并显示在 Web“未上报告警”中。默认发件箱上限为 1 GiB、磁盘保留空间为 512 MiB，达到任一阈值会删除最早的未上报事件；可用 `ALARM_STORE_MAX_BYTES` 和 `ALARM_STORE_MIN_FREE_BYTES` 覆盖。

事件视频直接从解码源帧建立缓冲，输出原始可见分辨率 MP4，不含音频。最小接入示例见 `rk3588_yolo/src/logic/logic_upload.cpp`；复用正式实现的单元测试见 `rk3588_yolo/tests/test_alarm_report/`。
