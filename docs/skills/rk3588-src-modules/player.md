# `src/player`：显示、叠加、文本与 RTSP 输出

## 文件职责

- `display.cpp/.h`：拼接缓冲分配/显示锁、GTK 显示、统一 overlay 渲染和单通道画面渲染。
- `text_overlay.cpp/.h`：OpenCV freetype 的 UTF-8/中文文字。
- `rtsp_streamer.cpp/.h`：把完整拼接前缓冲编码成 RTSP。

## 显示路径

analyzer 的每通道 display worker 将最新源帧缩放到 tile，叠加内容后提交到全局拼接缓冲。GTK 和 RTSP 读取同一张含叠加的拼接画面；所以 `enable_display=false`、`enable_rtsp=true` 时仍必须分配并持续合成该缓冲。

`display_lock()`/`display_unlock()` 防止 GTK/RTSP 读取时撕裂。调用者必须缩短持锁时间，不能在锁内做编码或网络发送。

## `RenderParams`

真实字段是通道 id、模型输入宽高、display/infer FPS、结果年龄、FPS 开关、target mask、系统/自定义 overlay 开关，以及 ROI/results/draw_cmds 指针。它没有旧文档中的 `srcWidth/srcHeight` 字段。

`render_overlays()` 的目标图是任意尺寸 BGR Mat。它按 `screen/inputW,inputH` 直接映射检测框、ROI 和绘制指令；因为 analyzer 已把源帧整幅缩放到模型输入，坐标无需再经过源分辨率换算。

系统叠加包含 ROI、检测框、姿态/分割等；自定义叠加是 logic 的 draw commands。`target_mask` 控制 DISPLAY/IMAGE/VIDEO，`show_system_overlays` 与 `show_custom_overlays` 再控制两类内容。`render_channel_view()` 可在不访问 framebuffer 的情况下生成与单通道实时窗口一致的 BGR 画面，供报警录像使用。

## 文字

`draw_text_unicode()` 使用 OpenCV freetype；字体加载顺序为 `RK_OVERLAY_FONT`、项目 `assets/fonts/overlay.*`、系统文泉驿字体。字体不可用时函数返回 false，调用方决定是否用 `cv::putText` 回退。文字颜色仍是 BGR。

## RTSP

RTSP 服务在独立 GMainContext/线程运行，地址由 `global.rtsp_port` 和 `rtsp_path` 决定。编码 `auto` 优先 mpp 硬编再回退软件，`hw` 强制硬编；codec 支持 H.264/H.265。服务启动失败时 main 视为致命错误，避免客户端连到占用端口的旧进程。

修改 tile 或输出尺寸时需注意显示缓冲在启动时分配，属于应重启的结构配置。新增 overlay 时优先扩展 `DrawCommand` 和统一 `render_overlays()`，确保 GTK、RTSP、告警图片和告警视频规则一致，避免各输出各画一套。
