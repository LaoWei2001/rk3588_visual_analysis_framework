# 主程序与进程运行时

对应 `src/main.cpp`、`src/system.h`。

## 启动顺序

`main()` 依次：提高 `RLIMIT_NOFILE`；调用 `app_ctrl_init()` 加载配置；初始化 GStreamer；注册信号；
初始化暂停控制和拼接缓冲；调用 `analyzer_init()`、`channel_control_init()`；创建/复用采集器，再创建
显示和结果分发线程；启动 RTSP；最后才创建配置监控和 fd 监控线程，进入 GTK 或无界面主循环。
配置监控延后启动是为了避免启动阶段与 main 同时修改 capturer/固定拓扑。

采集器内部创建 bus/reconnect 线程，推理引擎内部创建 infer worker，全局 logic 线程在 `analyzer_init()` 间接启动。告警图片和录像 worker 为首次使用时惰性创建。

## 信号与运行控制

- `SIGINT`/`SIGTERM`：置停止标志并唤醒等待线程。
- `SIGUSR1`：调用 `pause_ctrl::toggle()`；只有配置启用暂停键时键盘空格才参与控制。
- `SIGPIPE`：忽略，由网络模块处理写失败。
- `DBG_PRINT`：定义在 `system.h`，受 `global.debug_display` 控制；调用文件须能看到 `g_pCtrl` 的完整定义。

## 退出顺序不可随意打乱

当前顺序为：停止 RTSP；请求全局停止、解除暂停并停控制 socket；先 join 配置/fd 监控，防止退出中
重建模型或采集器；调用 `analyzer_request_stop()` 唤醒推理/分发；停止并删除采集器；join dispatch
和 display；再调用 `analyzer_deinit()` 释放推理、tracker 和 logic；依次停止录像、事件图片 worker；
销毁显示队列/缓冲，最后 `app_ctrl_deinit()`。

模块新增线程时必须提供可重复调用的停止入口、唤醒阻塞等待、join 后再销毁其锁/条件变量/依赖资源，并在此退出链中放到正确位置。

## 共享视频源

`main()` 会按规范化后的 `src_type + location` 复用已有 `DecChannel`，一个采集器可向多个逻辑通道分发同一源。修改采集初始化或热切流时必须保留这一语义。
