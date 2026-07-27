# vision_analysis 当前架构图

> 配套说明：[vision_analysis_系统说明文档.md](vision_analysis_系统说明文档.md)

本文只描述当前 `vision_analysis/src/` 实现。图中 C++ 数组旁的 `ch/slot` 指内部运行槽位；
HTTP、Socket、告警清单和 `ctx->chnId` 中的 `channel_id` 均指 `config.channels[].id`。

## 1. 总体模块图

```text
 Web Console
 ├─ 画布编辑 config.json / report_policy / ROI
 ├─ 实时画面与系统控制
 └─ 自定义按钮
          │ HTTP
          ▼
 ┌──────────────────────── web_console backend ────────────────────────┐
 │ 配置读写、进程管理、控制请求转发                                    │
 └───────────────┬───────────────────────────────┬─────────────────────┘
                 │ config.json                   │ Unix Socket
                 ▼                               ▼
 ┌──────────────────────── vision_analysis process ────────────────────┐
 │                                                                    │
 │  config/ + core/                   control/channel_control          │
 │  ├─ 配置解析                       ├─ 系统动作                       │
 │  ├─ REG_G / REG_C                  └─ 每通道业务动作 FIFO            │
 │  └─ 配置热重载                              │                       │
 │           │                                  ▼                       │
 │           ├──────────────▶ analyzer/channel_pipeline                 │
 │           │                         │                                │
 │  capturer/decChannel                ├─ action handler                │
 │  RTSP / USB / 文件 ────────────────▶├─ channel logic                 │
 │           │                         └─ DrawCommand / report_alarm     │
 │           ▼                                  │                       │
 │  analyzer/frame_inlet                       ├────────▶ player/display │
 │  ├─ 最新显示帧                              ├────────▶ rtsp_streamer │
 │  ├─ 录像源帧缓存                            ├────────▶ alarm_report  │
 │  └─ 节流后提交 RKNN                         └────────▶ event_video   │
 │           │                                                          │
 │           ▼                                                          │
 │  yolo / infer workers → result dispatch → tracker                    │
 │                                                                    │
 └────────────────────────────────────────────────────────────────────┘
```

当前 C++ 源码没有 `src/uploader/`。业务 logic 不再调用 `alarm_uploader_enqueue()` 或向 Redis 直接写告警。

## 2. 单通道帧与结果流

```text
GStreamer appsink
      │ 原始源帧，src_width × src_height
      ▼
frame_inlet(ch)
      ├──────────────────────────────┐
      │                              │
      │ 最新帧                       │ 原始帧历史
      ▼                              ▼
display_worker(ch)         event_video_recorder ring buffer
      │
      └─ 使用最近结果叠加后显示

frame_inlet(ch)
      │ max_fps 节流、缩放/预处理
      ▼
infer queue → RKNN infer worker
      │
      ▼
dispatch_worker(ch)
      ├─ tracker
      ├─ 构造 ChannelContext
      ├─ channel_control_take(ch)
      ├─ 执行当前 logic 的 action handler
      ├─ 执行当前 channel logic
      └─ 写回 results / draw_cmds / logic_state
```

`ChannelContext::frame` 和检测框位于模型输入坐标系；`src_width/src_height` 才是原始视频分辨率。

## 3. 通道隔离

```text
APP_CTRL
├─ config.channels[0] ─────┐
├─ channels_state[0]       ├─ ChannelContext(ch=0)
├─ chn_mtx[0]              │
└─ process_mtx[0] ─────────┘

├─ config.channels[1] ─────┐
├─ channels_state[1]       ├─ ChannelContext(ch=1)
├─ chn_mtx[1]              │
└─ process_mtx[1] ─────────┘

...每个通道独立分槽
```

同一个 logic 函数可以被多通道复用，但 `ctx->config`、`ctx->results`、`ctx->state` 和 ROI 始终指向当前通道的数据。每通道跨帧状态必须放在 `ctx->state`，不能使用共享 `static`。

跨通道读取：

```text
logic(ch A)
   └─ ctx->get_channel_snapshot(ch B)
          └─ 在 chn_mtx[B] 内取得同帧 frame + results + fps + age
                 └─ 返回深拷贝快照
```

## 4. ROI 数据流

```text
Web ROI 节点
   │ 保存归一化坐标 0~1
   ▼
config.json / channels[ch]
   └─ roi_zones[]     多个命名区域
          │
          ▼
config.cpp 解析到 ChannelConfig
          │
          ▼
load_roi_zones_from_config()
          │ × 模型输入宽高
          ▼
运行时 roi_zones（模型坐标系）
          ├─ ctx->rois  全部区域
          └─ 显示/图片/视频叠加
```

配置热重载时：

```text
config mtime 变化
  → 重新解析
  → 显式复制 roi_zones
  → load_roi_zones_from_config()
  → 下一批逻辑帧使用新 ROI
```

ROI 只保存在当前运行配置的 `channels[].roi_zones[]` 中，并支持热更新。

## 5. Logic 注册与 Web 声明

```text
logic_xxx.cpp
  REGISTER_LOGIC(logic_xxx)
          │ 函数名字符串化为 "logic_xxx"
          │ 静态初始化
          ▼
  C++ channel logic 注册表
          ▲
          │ config.json: channels[ch].logic = "logic_xxx"
          │
channel_pipeline 查表并执行
```

Web 识别链是独立的：

```text
App/logics.json channel_logics[]
          │ GET /apps/{name}/logics
          ▼
NodeConfigPanel 下拉和动态参数表单
          │ 保存
          ▼
config.json channels[ch].logic + 参数
```

非空时，构建器从 `REGISTER_LOGIC(logic_xxx)` 的函数名生成 `logics.json.name`，通道配置的 `logic` 使用同一名称。源 `logic.json` 不手写 `name`。配置不写 `logic` 时框架直接跳过业务后处理；未知非空名称会被 Schema 校验拒绝。Web 优先读取应用 `logics.json`，缺失时才通过二进制 `--list-logics` 获取名称。

## 6. 告警上报链

```text
channel logic
   │ report_alarm(ctx, type, message, fields)
   ▼
alarm_report
   │ 读取本通道 report_policy_json / report_parameters_json
   │
   ├─ 图片 delivery
   │    └─ alarm_image_worker
   │         ├─ 按策略选择画面与 overlay
   │         ├─ 编码/落盘
   │         └─ 写事件清单
   │
   └─ 视频 delivery
        └─ event_video_recorder_trigger
             ├─ 报警前环形缓存
             ├─ 报警后继续收帧
             ├─ 按 video_fps 异步编码 MP4
             └─ alarm_report_video_ready
```

上报目标和媒体由 `report_policy` 决定，不再使用通道级 `report_enable/server_url/dify_api_url` 字段。

## 7. 绘制目标路由

```text
logic draw_*()
      │ DrawCommand::Target
      ├─ DISPLAY ─────────▶ 实时显示 / 监看
      ├─ IMAGE ───────────▶ 告警图片
      ├─ VIDEO ───────────▶ 事件视频
      ├─ UPLOAD ──────────▶ IMAGE + VIDEO
      └─ ALL ─────────────▶ DISPLAY + IMAGE + VIDEO
```

`report_policy` 还会进一步决定图片或视频是否启用系统叠加、自定义叠加以及使用原始源帧还是通道显示画面。

## 8. 自定义按钮控制链

```text
实时画面打开
  → Web 获取当前 logic 的 actions
  → 用户点击按钮
  → POST 控制 API
  → backend 连接 vision_analysis Unix Socket
  → channel_control 解析请求
       ├─ 系统动作：直接执行上下线/重连等操作
       └─ 业务动作：记录 channel_id + logic_name + action + payload
                         │
                         ▼
                   每通道 FIFO（最多 64）
                         │
                         ▼
               下一次该通道处理逻辑帧
                         │
                         ├─ logic 名未变化：调用 action handler
                         └─ logic 已变化：丢弃旧动作
```

HTTP `accepted` 只代表成功进入队列。handler 是否处理成功要看后续执行结果和日志。

## 9. 配置热重载

```text
config_monitor_thread
  │ 检测 config.json mtime 并等待写入稳定
  ▼
load_app_config(new_cfg)
  ├─ REG_G / REG_C 字段 → sync_fields
  ├─ ROI → 显式复制并重建
  ├─ logic 改变 → 切换名称并 reset logic_state
  ├─ global_logics 改变 → 重启 global logic threads
  ├─ model 改变 → 热换模型
  ├─ tracker/阈值/类别 → 更新运行模块
  └─ stream 改变 → 停止并重建相关采集器
```

新增模块专有业务参数时，只修改模块 `logic.json.parameters` 和同目录 C++ 的 `ctx->param_*()` 调用。Schema 在构建时驱动二进制校验、Web 表单和热重载策略；普通参数不再增加 `ChannelConfig/REG_C` 中央字段。

## 10. 线程关系

```text
main thread
├─ config_monitor_thread                 ×1
├─ fd_monitor_thread                     ×1
├─ capture_bus_thread                    ×采集器
├─ display_worker                        ×启用显示的通道
├─ dispatch_worker                       ×通道/调度配置
├─ infer_worker                          ×模型配置
├─ global_logic thread                   ×启用的全局逻辑实例
├─ channel_control server thread         ×1
├─ rtsp loop + feeder thread             启用 RTSP 推流时
├─ alarm_image_worker                    首次告警图片任务时
└─ event_video_worker                    首次启用事件录像时创建固定大小的 worker pool
```

此外存在 GStreamer、RKNN 等库内部线程。不要用固定数字推断实际线程总数。

## 11. 启动与退出

启动：

```text
app_ctrl_init
  → GStreamer/硬件环境
  → analyzer_init
       → infer workers / ROI / global logics
  → channel_control_init
  → config + fd monitors
  → DecChannel
  → display + dispatch workers
  → rtsp_streamer_init（按配置）
  → main loop
```

退出：

```text
设置停止标志并唤醒等待者
  → rtsp_streamer_deinit
  → channel_control_deinit
  → 停止通道采集器
  → analyzer_deinit 并等待分发线程
  → 唤醒并等待显示线程
  → event_video_recorder_deinit
  → alarm_report_deinit
  → 停监控线程
  → app_ctrl_deinit
```

原则是先停止上游生产者和外部入口，再停止下游 worker，最后销毁共享状态。
