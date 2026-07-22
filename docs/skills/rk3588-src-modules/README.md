# rk3588_yolo `src/` 模块索引

> 文档角色：C++ 源码参考索引，供已经明确模块边界的开发者深入查阅；不是项目总入口。上级导航：[docs 文档总入口](../../README.md) · [开发/运维知识库索引](../README.md)。

本目录按当前 `rk3588_yolo/src/` 源码整理，供开发者查架构，也供大模型在二次开发前按需读取。路径均相对 `rk3588_yolo/`。不要把本文当作配置 Schema；字段真值以 `src/config/config.h`、`config_init.cpp` 和 `config_validator.cpp` 为准。

## 模块地图

| 源码范围 | 职责 | 文档 |
|---|---|---|
| `src/main.cpp`、`src/system.h` | 进程启动、线程创建、信号和逆序退出、公共日志宏 | [runtime.md](runtime.md) |
| `src/config/` | JSON 加载、校验、字段注册和热重载数据模型 | [config.md](config.md) |
| `src/core/` | 全局控制块、通道状态、线程安全查询、暂停和图片工具 | [core.md](core.md) |
| `src/capturer/` | RTSP、文件、USB 的 GStreamer 采集、共享和重连 | [capturer.md](capturer.md) |
| `src/analyzer/` | 帧入口、RGA、推理调度、tracker、logic 和显示分发 | [analyzer.md](analyzer.md) |
| `src/yolo/` | RKNN 模型实现及多模型结果合并 | [yolo.md](yolo.md) |
| `src/logic/` | 通道逻辑、全局逻辑、ROI/绘制 API 和业务实现 | [logic.md](logic.md) |
| `src/control/` | Web/外设动作经 Unix Socket 投递到通道逻辑 | [control.md](control.md) |
| `src/alarm/` | 告警事件建档、图片生成、投递清单和事件合并 | [alarm.md](alarm.md) |
| `src/recorder/` | 告警前后帧缓存与 MP4 编码 | [recorder.md](recorder.md) |
| `src/player/` | 拼接显示、统一叠加、UTF-8 文本和 RTSP 推流 | [player.md](player.md) |
| `src/third_party/` | GStreamer buffer 适配、RK MPI 声明、系统工具和 cJSON | [third_party.md](third_party.md) |
| 外部上传服务 | 消费 `alarm_store` 发件箱并发送到服务器/Dify；不是当前 `src/` 模块 | [uploader.md](uploader.md) |

`src/third_party/` 不按业务模块扩展；修改或升级它时，应核对许可证、ABI 和所有调用方。

## 当前端到端链路

```text
DecChannel/appsink
  -> videoOutHandle/frame_inlet
     -> 事件录像源帧预缓存
     -> 显示单槽队列 ---------------------------> display_worker -> 拼接缓冲 -> GTK/RTSP
     -> RGA 转模型输入 -> algorithm_process_mat
                           -> infer worker -> result_dispatch
                              -> tracker
                              -> 先消费通道动作，再执行 channel logic
                              -> 原子写回同帧 frame/results/draw_cmds/state
                                      -> report_alarm
                                         -> alarm_store/<event_id>/manifest.json
                                            + raw.jpg / snapshot.jpg
                                         -> event_video_recorder -> clip.mp4
                                         -> 外部上传服务消费发件箱
```

没有启用推理的通道仍会解码、显示并逐帧调用 logic，只是 `ctx->results` 为空。显示使用最新源帧并允许复用较旧推理结果；业务 logic、告警图片和快照使用与推理结果严格匹配的模型输入帧。

## 全局约定

- `chnId/channel_id/ChannelConfig.id` 是同一个值，也直接作为固定通道数组索引。配置加载后按 ID 排序，排序位置只用于显示布局；热重载禁止改变通道数量或 ID。
- 检测框、ROI、`ctx->frame` 和 `draw_*` 均在模型输入坐标系中，通常为 640×640。预处理为整幅缩放，不是 letterbox。
- `ctx->timestamp_ms` 是单调时钟，只算间隔；`ctx->unix_ms` 是 Unix epoch 毫秒，用于日历时间和上报。
- 颜色使用 OpenCV BGR：`cv::Scalar(B, G, R)`。
- `DrawCommand::DISPLAY`、`IMAGE`、`VIDEO` 是绘制目标位；`UPLOAD=IMAGE|VIDEO`，`ALL` 包含三者。当前告警“叠加画面”会复用实时层，因此图片按 `DISPLAY|IMAGE`、视频按 `DISPLAY|VIDEO` 取命令；纯原始媒体不绘制任何命令。
- 配置由 `g_pCtrl->mtx`（pthread rwlock）保护，通道共享状态由 `chn_mtx[chnId]` 保护。跨通道业务代码优先使用 `get_channel_snapshot()`。

## 按任务选择文档

| 任务 | 先读 |
|---|---|
| 新增或修改通道 logic | [logic.md](logic.md)、`../rk3588-channel-logic/` |
| 新增 Web 自定义按钮/外部动作 | [control.md](control.md)、[logic.md](logic.md) |
| 新增配置字段或理解热重载 | [config.md](config.md)、[core.md](core.md) |
| 接入新模型 | [yolo.md](yolo.md)、[analyzer.md](analyzer.md) |
| 接入新视频源或调整重连 | [capturer.md](capturer.md) |
| 修改叠加或输出画面 | [player.md](player.md)、[logic.md](logic.md) |
| 触发告警并生成图片/视频 | [alarm.md](alarm.md)、[recorder.md](recorder.md) |
| 修改进程线程或退出顺序 | [runtime.md](runtime.md) |
| 更换 GStreamer/RK MPI/第三方基础适配 | [third_party.md](third_party.md) |

## 文档维护规则

新增、删除或移动 `src/<module>` 时同步更新本表；修改公共结构/API 时同步更新对应模块页。示例必须来自当前头文件或真实调用点，不能保留已经删除的函数名。
