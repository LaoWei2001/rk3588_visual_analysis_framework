# vision_analysis 系统说明文档

> 配套图示：[vision_analysis_架构图.md](vision_analysis_架构图.md)

本文以当前 `vision_analysis/src/` 实现为准，说明进程内模块、线程、数据流、配置热重载、告警上报和生命周期。二次开发时应优先以本文列出的权威头文件和实际源码为准。

## 1. 系统概述

`vision_analysis` 是运行在 RK3588 上的多通道视频分析进程。每个通道可以使用 RTSP、USB 摄像头或视频文件，经过解码、预处理、RKNN 推理、跟踪和业务 logic 后，分别进入实时显示、内置 RTSP 推流、告警图片或事件视频路径。

系统的核心原则：

- 通道运行时编号是 `config.channels[]` 的数组下标；
- 每通道配置、检测结果、logic 状态、ROI 和绘制指令相互隔离；
- 推理和显示通过队列、快照及互斥量解耦；
- channel logic 只表达业务事件，不直接实现上报服务；
- Web 画布保存为 `config.json`，大部分参数可运行时热更新；
- 自定义按钮通过 HTTP → Unix Socket → 通道动作队列进入当前 logic 的 action handler。

## 2. 当前模块划分

| 目录 | 职责 | 关键文件 |
|---|---|---|
| `capturer/` | GStreamer 解码、帧采集、bus 监听和断流重连 | `decChannel.*` |
| `analyzer/` | 帧入口、模型调度、推理结果分发、跟踪和 channel logic 调用 | `analyzer.*`、`frame_inlet.*`、`channel_pipeline.*` |
| `yolo/` | RKNN 模型推理与不同 YOLO 输出解析 | `yolo.*`、各模型实现 |
| `logic/` | channel logic、global logic、自注册表和业务实现 | `channel_logic.*`、`global_logic.*`、`logic_*.cpp` |
| `player/` | 叠加渲染、显示和内置 RTSP 推流 | `display.*`、`rtsp_streamer.*` |
| `alarm/` | 统一告警入口、图片和事件清单异步落盘 | `alarm_report.*` |
| `recorder/` | 报警前后帧缓存和事件 MP4 异步编码 | `event_video_recorder.*` |
| `control/` | Web 控制 Unix Socket、系统动作和每通道业务动作队列 | `channel_control.*` |
| `config/` | JSON 配置解析、字段注册和热拷贝 | `config.*`、`config_registry.*`、`config_init.cpp` |
| `core/` | `APP_CTRL`、每通道运行状态、配置监控和暂停控制 | `app_ctrl.*`、`pause_ctrl.*` |

当前源码中没有 `src/uploader/`。C++ 不再通过 `alarm_uploader_enqueue()` 或 Redis 直接提交业务告警。

## 3. 通道数据流

单通道主路径如下：

```text
RTSP / USB / 文件
    ↓
DecChannel + GStreamer
    ↓ 原始源帧
frame_inlet
    ├─ 推送事件录像源帧环形缓存
    ├─ 更新实时显示所需最新帧
    └─ 按 max_fps 节流并提交推理
          ↓
      RKNN infer worker
          ↓
      result dispatch
          ↓
      跟踪 + ChannelContext
          ↓
      自定义 action handler
          ↓
      channel logic
          ├─ DrawCommand → 显示 / 图片 / 视频
          └─ report_alarm → 告警图片 / 事件视频 / 事件清单
```

显示路径使用最新解码帧，并叠加最近的推理结果；告警图片和事件视频根据 `report_policy` 决定使用原始画面、显示画面和哪些叠加信息。

## 4. 线程与后台任务

### 4.1 主处理线程

`main.cpp` 中列出的核心线程类别包括：

1. `config_monitor_thread`：监控配置文件变化；
2. `fd_monitor_thread`：监控进程文件描述符数量；
3. `capture_bus_thread[N]`：各解码器的 GStreamer bus 监听和重连；
4. `display_worker[N]`：异步显示；
5. `dispatch_worker[N]`：消费推理结果并执行跟踪和 channel logic；
6. `infer_worker[N]`：RKNN 推理 worker；
7. `global_logic[N]`：跨通道全局逻辑；
8. `alarm_image_worker`：告警图片和事件清单异步落盘，首次需要时创建；
9. `event_video_worker[N]`：事件视频编码，首次需要时按实现中的固定 worker-pool 大小创建。

此外还有两个不应遗漏的后台组件：

- `channel_control` 使用独立 `std::thread` 监听 Unix Socket；
- `rtsp_streamer` 启用时创建 GLib 主循环线程和视频 feeder 线程。

GStreamer、RKNN 或底层库还可能创建自己的内部线程，因此“线程总数”不能简单等于上述类别数。

### 4.2 同步边界

| 同步对象 | 保护内容 |
|---|---|
| `APP_CTRL::mtx` | 全局配置和通道配置热更新 |
| `chn_mtx[i]` | 第 `i` 个通道的帧、结果、logic 状态、绘制指令和统计数据 |
| `g_process_mtx[i]` | 同一通道结果处理和 logic 执行串行化 |
| 配置条件变量 | 配置监控线程的定时等待和退出唤醒 |
| 告警图片队列锁/条件变量 | 图片及清单后台任务 |
| 事件录像队列锁/条件变量 | 录像任务和帧缓存 |
| 控制队列锁 | 每通道自定义按钮动作 FIFO |

跨通道读取必须通过 `ChannelContext::get_channel_snapshot()`，它会在通道锁内取得同一时刻的帧、结果和统计信息，并返回可安全持有的深拷贝。

## 5. ChannelContext 与业务逻辑

每次执行 channel logic 时，框架在当前处理栈上构造一个 `ChannelContext`，将字段指向当前通道的数据槽：

- `frame`、`results`：本通道当前逻辑帧和检测结果；
- `config`：本通道只读配置；
- `roi`、`rois`：模型输入坐标系中的单/多 ROI；
- `draw_cmds`：本帧自定义绘制输出；
- `state`：本通道跨帧持久状态；
- `src_width/src_height`：原始视频分辨率；
- `timestamp_ms/unix_ms`：单调计时和真实日历时间。

logic 文件通过以下宏在 `main()` 前注册：

```cpp
REGISTER_LOGIC(logic_xxx);
```

宏会把函数标识符字符串化，它同时成为运行时查找、config、Web 和外部 API 使用的 logic ID。配置未声明 `logic` 时框架直接跳过业务调用；未知的非空名称会在嵌入 Schema 校验阶段被拒绝，分发表查找失败也返回空指针而不冒充某个默认模块。Web 名称由构建器从同一函数名生成。

跨帧、每通道状态必须放入 `ctx->state`。不要使用函数内 `static` 或无保护的全局变量，否则多个通道会共享同一状态。

## 6. ROI

当前 C++ 从每个通道的 `config.json` 中读取：

- `roi_zones`：多个带名称的归一化多边形；
- `roi_polygon`：旧单区域兼容字段。

加载时，归一化坐标乘以模型输入尺寸，生成 `ChannelState::roi_zones`。当前运行路径不依赖外部 `roi_zones.json`。

配置监控检测到文件变化后会显式复制 ROI 字段，并调用 `load_roi_zones_from_config()` 重建运行时 ROI，因此 ROI 支持热更新，无需停止程序。

`ctx->roi` 指向第一个区域，仅用于兼容旧逻辑；新逻辑应使用 `ctx->rois`、`roi_find()`、`roi_contains()` 等多区域 API。`ROI_ALL=-1`，`ROI_NONE=-2`。

## 7. 配置加载与热重载

### 7.1 注册字段

普通全局和通道字段通过 `REG_G`、`REG_C` 注册。配置初次加载时注册表负责解析，热重载时 `sync_fields()` 按注册类型复制。

支持的 `ConfigType`：

- `STRING`
- `INT`
- `FLOAT`
- `BOOL`
- `STRING_ARRAY`
- `JSON`

模块专有业务参数在该模块 `logic.json.parameters` 声明，通过 `ctx->param_*()` 读取。构建时 Schema 同时生成二进制类型化校验和 Web 清单，参数值按声明的 `preserve_state`、`reset_state` 或 `restart_required` 策略参与热重载，不再扩展中央 `ChannelConfig/REG_C` 字段。

### 7.2 特殊热更新

以下内容不完全依赖注册表，而由配置监控显式处理：

- `roi_zones/roi_polygon`：复制后重建运行时 ROI；
- `models`：比较模型配置并按需重新加载；
- `stream`：检测源类型、地址、USB 参数等变化并重建采集器；
- `logic`：切换运行逻辑并清理该通道旧 `logic_state`、结果和绘制状态；
- `global_logics`：数组变化时重启全局逻辑线程；
- 跟踪阈值和类别过滤：同步到运行中的推理/跟踪模块。

通道数量等改变整体内存布局的配置不应假设能够原地热更新，具体以 `app_ctrl.cpp` 的比较和处理逻辑为准。

## 8. 告警上报

### 8.1 统一入口

业务逻辑唯一推荐入口：

```cpp
report_alarm(ctx, "alarm_type", "message", {
    alarm_field("score", score),
    alarm_field("track_id", track_id),
});
```

logic 只提交事件类型、消息和运行时字段。以下内容由 Web 保存的 `report_policy` 决定：

- 图片、视频或两者；
- 图片/视频使用哪种画面；
- 是否叠加系统信息和自定义绘制；
- 事件录像前后时长、帧率；
- 投递 Profile；
- 输出 JSON 字段映射。

当前配置字段是 `report_policy_json` 和 `report_parameters_json`，没有旧的 `report_enable/server_url/dify_api_url/dify_api_key` 通道字段。

### 8.2 图片路径

`alarm_report` 根据策略生成图片任务。后台 `alarm_image_worker` 负责渲染、编码、落盘并写事件清单，避免在推理线程内进行耗时 I/O。

### 8.3 事件视频路径

`event_video_recorder` 持续保存需要的源帧历史。告警触发后组合报警前缓存和报警后帧，异步编码 MP4；完成后调用 `alarm_report_video_ready()` 更新事件。

### 8.4 绘制路由

`DrawCommand::Target`：

- `DISPLAY`：实时显示；
- `IMAGE`：告警图片；
- `VIDEO`：事件视频；
- `UPLOAD`：图片和视频；
- `ALL`：全部目标。

## 9. Web 自定义按钮控制链

控制链为：

```text
实时画面按钮
  → Web HTTP API
  → 后端连接进程 Unix Socket
  → channel_control
  → 对应通道 FIFO
  → 下一次该通道处理逻辑帧时执行 action handler
```

系统动作如上下线、重连等由 `channel_control` 直接处理；业务动作记录入队时的 logic 名称，消费时若通道已经切换到其他 logic，会丢弃旧动作，防止错误投递。

HTTP 返回 `accepted` 只表示动作成功进入队列，不代表业务 handler 已执行成功。详细开发方法见 `custom-button-actions.md`。

## 10. 启动生命周期

当前主流程可概括为：

```text
解析参数和配置
→ app_ctrl_init
→ GStreamer 初始化
→ 硬件/显示环境初始化
→ analyzer_init
   ├─ tracker 初始化
   ├─ algorithm_init / infer workers
   ├─ ROI 加载
   └─ global logic 启动
→ channel_control_init
→ config monitor / fd monitor
→ 各通道 DecChannel 初始化
→ display workers / dispatch workers
→ rtsp_streamer_init（按配置）
→ main 事件循环
```

告警图片和事件录像 worker 是按需创建，不要求在启动阶段预先存在。

## 11. 退出生命周期

收到退出信号后设置运行标志并唤醒等待线程，随后停止外部入口并逆序释放：

```text
停止 RTSP streamer
→ 停止 channel_control，设置全局退出标志
→ 停止通道采集器
→ 停止 analyzer（推理、global logic、跟踪等）并等待分发线程
→ 唤醒并等待显示线程
→ 停止 event_video_recorder
→ 停止 alarm_report worker
→ 停止配置与 fd 监控
→ app_ctrl_deinit
```

修改生命周期时必须确保生产者先停止，再停止其下游消费者，最后销毁共享状态和同步对象。

## 12. 二次开发检查清单

### 新增 channel logic

1. 新建独立 `src/logic/modules/logic_xxx/`，包含 `logic.cpp` 和 `logic.json`；
2. `#include "logic/core/logic_common.h"`；
3. 使用 `ctx->state` 保存跨帧状态；
4. 文件末尾 `REGISTER_LOGIC(logic_xxx)`，函数名即外部 logic ID；
5. 同目录 `logic.json` 声明参数、动作和上报字段，不写 `name`；正常打包自动生成 App `logics.json`；
6. 如需上报，调用 `report_alarm()`，由画布配置 `report_policy`；
7. 如需按钮，实现 `REGISTER_LOGIC_ACTION(logic_xxx, handler)` 并在模块 `logic.json` 声明 actions。

### 新增配置参数

1. 在模块 `logic.json.parameters.properties` 声明类型、默认值、范围和热重载策略；
2. 在同模块 C++ 使用匹配的 `ctx->param_*()` 访问器；
3. 不为普通模块参数扩展 `ChannelConfig/REG_C`，不手改生成的 `logics.json`；
4. 运行状态放 `ctx->state`，不要缓存参数副本。

### 常见禁止项

- 不要引用已删除的 `alarm_uploader`；
- 不要把服务地址或密钥硬编码进 logic；
- 不要从外部 `roi_zones.json` 读取当前运行 ROI；
- 不要用 `static` 保存每通道状态；
- 不要直接读取其他通道的内部状态，使用 `get_channel_snapshot()`；
- 不要把 HTTP `accepted` 当成业务动作完成。
