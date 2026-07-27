# `src/recorder`：告警事件录像

`event_video_recorder` 保存每通道报警前的环形帧，并在触发后继续收集报警后帧，最后编码 H.264 MP4。源帧入口位于 analyzer 的 `frame_inlet`，因此不依赖 GTK 或 RTSP 是否开启。

## 配置与模式

通道配置默认值为前 3 秒、后 3 秒、15 FPS，来自 `ChannelConfig`；运行时还会优先读取 `report_policy` 中的 `video_pre_sec`、`video_post_sec`、`video_fps`。秒数限制为 0–120，FPS 限制为 1–30。

`EventVideoOverlayMode`：

- `NONE`：原始源帧。
- `CUSTOM`：只绘制 target 含 `VIDEO` 的 logic 自定义指令。
- `ALL`：系统检测/ROI与 target 含 `VIDEO` 的自定义指令都绘制。
- `DISPLAY`：先缩放到该通道实时 tile 大小，再绘制系统层以及 target 含 `DISPLAY|VIDEO` 的自定义指令；不读取 framebuffer。

上述是 recorder 内部枚举的完整能力。当前 frame inlet 对配置值 `custom` 和 `all` 都映射为 `DISPLAY`，以实现 Web 的“与实时播放窗口画面一致/画面上叠加信息”；`none` 映射为原始模式。若以后要让 Web 分别暴露 CUSTOM/ALL，必须同步修改该映射，而不能只改枚举说明。

用于验证 Web 按钮触发视频上报的可运行模块是 `logic_upload_teach`，操作说明见 `../rk3588-channel-logic/references/examples/logic_upload_teach.md`。

## 工作模型

- 每通道按目标 FPS 节流并保留前窗环形缓存。
- 原始帧先进入每通道容量 4 的有界队列；满时丢旧帧，避免反压采集线程。
- 3 个共享 worker 公平轮询通道，同一通道保持帧顺序。
- 同时最多 1 个 MP4 编码任务，其余 worker 可继续处理源帧。
- 同通道同报警类型再次触发会延长现有后窗；断流时 `event_video_recorder_channel_offline()` 截断尚未结束的后窗。
- 编码器依次尝试 `mpph264enc`、`x264enc`、`openh264enc`，用 `mp4mux faststart=true` 封装。

## 公共接口边界

`event_video_recorder_push_source_frame()` 仅由帧入口持续喂帧；`event_video_recorder_trigger()` 由 alarm 模块触发；`event_video_recorder_extend()` 合并连续报警；编码成功后模块调用 `alarm_report_video_ready()`。业务 logic 不应直接拼装录像路径或自己调用 GStreamer 编码。

`EventVideoRequest` 自身的结构默认值是兜底值，不等同于产品配置默认值；正常调用由 alarm 模块显式填入当前策略中的 3/3/15 等参数。
