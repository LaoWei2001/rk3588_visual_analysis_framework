# `src/capturer`：GStreamer 视频采集

`DecChannel` 封装一个实际视频源和它的 bus/reconnect 线程；同一 `src_type + location` 的多个逻辑通道共享同一个实例，appsink 收到一帧后向其 `chnIds` 逐一调用 `videoOutHandle()`。

## 三种管线

- RTSP：`rtspsrc -> depay -> parse -> mppvideodec -> appsink`，按 `video_enc` 选择 H.264/H.265 元件。创建硬解前先做 RTSP TCP 快速探测；appsink `max-buffers=2, drop=true`，rtspsrc latency 为 100 ms。
- 文件：`filesrc -> decodebin -> appsink`，decodebin 动态连接解码后视频 pad；appsink `max-buffers=8, drop=true`。`loop=true` 时 EOS 回到开头，否则按离线/重连流程处理。
- USB：`v4l2src -> videoconvert -> capsfilter(NV12) -> appsink`，设备来自 `stream.device`。显式 `usb_width/usb_height` 固定采集分辨率；否则根据目标 FPS 选择设备档位和分辨率。目标 FPS 会限制在 1–30，实际相机档位为 30/15/10/5 等离散值，推理层仍独立按 `max_fps` 节流。

本模块当前没有从配置读取任意 USB caps 列表；接新摄像头时应核对其 `v4l2-ctl --list-formats-ext` 能力，再调整候选 caps。

## appsink 回调

回调读取 GstBuffer、视频格式、可见宽高、stride 和 DMA-BUF fd，构造 `ImgDesc_t` 后同步调用 analyzer。文件/通道的 `playback_fps` 在回调中以“丢帧不等待”方式限速；文件未显式配置时使用 `global.local_default_fps`。

回调返回后 GstBuffer/FD 生命周期结束。需要跨线程使用物理缓冲时，analyzer 必须在回调内导入稳定 RGA handle，不能保存裸指针或裸 fd。

## 断流和重连

bus 线程处理 warning、error、EOS、pipeline 状态以及长时间无 sample。重连前标记所有目标通道离线、停止旧 pipeline，再按阶梯退避重建；成功后的首帧恢复在线状态。文件、USB 与 RTSP 的处理存在分支，不应把 RTSP TCP 探测套到本地源。

`stop()` 先置停止标志，再让 pipeline 进入 NULL 并等待 bus 线程；代码为 mppvideodec 偶发阻塞提供多轮清理。销毁 `DecChannel` 前必须先 `stop()`，不能 detach 仍可能访问对象的线程。

## 新增源类型

需要同时修改 `StreamConfig`/解析/校验、`config_utils::is_supported_src_type()`、`DecChannel::init()` 分派、管线创建、热切流重建逻辑和 Web 序列化。还必须定义 location 的唯一性规则，否则共享采集器会错误合并或重复创建。
