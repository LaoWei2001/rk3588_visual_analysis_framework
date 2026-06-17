# rk3588_yolo 系统说明文档

> 配套图示：[rk3588_yolo_架构图.md](rk3588_yolo_架构图.md)
>
> 本文档详解系统结构、**各线程设计**、同步模型、关键数据结构与帧-结果时序保证。
> 内容以源码为准（`rk3588_yolo/src/`，路径均相对**仓库根**）。文末「近期架构增强」一节补充了 2026-06 落地的变化（每通道上报地址 / ROI 坐标系 / USB 采集分辨率），写新代码前请一并看。

---

## 目录

1. [系统概述](#1-系统概述)
2. [模块划分](#2-模块划分)
3. [线程设计详解](#3-线程设计详解)（核心）
4. [同步原语清单](#4-同步原语清单)
5. [关键数据结构](#5-关键数据结构)
6. [帧-结果时序保证](#6-帧-结果时序保证)
7. [生命周期：启动与退出](#7-生命周期启动与退出)
8. [硬件与环境约束](#8-硬件与环境约束)

---

## 1. 系统概述

rk3588_yolo 是运行在 RK3588 板端的**多路视频 YOLO 推理程序**。单进程内并发处理多路视频流，每路独立配置流地址、模型、业务逻辑：

```
RTSP / 本地文件 / USB  →  GStreamer 硬解  →  RGA 格式转换  →  RKNN NPU 推理
                                                                  ↓
        屏幕 GTK 显示  ←  叠加渲染  ←  SORT 跟踪 + 业务逻辑（channel/global）
                                                                  ↓
                              异步上报（server 落盘发件箱 / dify 走 Redis）→ Python 微服务
```

设计风格为 **C/pthread**：所有线程在 `main.cpp` 中可循、生命周期显式管理、帧与结果用同一结构体在锁内原子配对。

---

## 2. 模块划分

| 目录 | 职责 | 关键文件 | 子文档 |
|------|------|----------|--------|
| `capturer/` | GStreamer 多路采集、自动重连 | `decChannel.cpp` | [capturer.md](../../rk3588-src-modules/capturer.md) |
| `analyzer/` | 推理调度核心：节流、转换、NPU 队列、跟踪、逻辑分发、显示 | `frame_inlet / algo_engine / channel_pipeline / result_dispatch / display_pipeline` | [analyzer.md](../../rk3588-src-modules/analyzer.md) |
| `yolo/` | RKNN 推理引擎（v5/v8-det/pose/seg） | `yolo.cpp / yolov8det.cpp / …` | [yolo.md](../../rk3588-src-modules/yolo.md) |
| `logic/` | 业务逻辑：单路 channel_logic、跨路 global_logic | `channel_logic.cpp / global_logic.cpp` | [logic.md](../../rk3588-src-modules/logic.md) |
| `player/` | GTK3 显示、RGA 缩放、叠加渲染 | `display.cpp` | [player.md](../../rk3588-src-modules/player.md) |
| `uploader/` | 告警异步上报（Redis 生产者-消费者） | `alarm_uploader.cpp` | [uploader.md](../../rk3588-src-modules/uploader.md) |
| `config/` | JSON 解析、注册表热重载 | `config.cpp / config_registry.cpp` | [config.md](../../rk3588-src-modules/config.md) |
| `core/` | APP_CTRL 全局控制块、通道状态、热监控 | `app_ctrl.cpp` | [core.md](../../rk3588-src-modules/core.md) |

---

## 3. 线程设计详解

系统共 **8 类自建线程**，外加 GStreamer 内部 streaming 线程和 main 主线程。下面逐一说明每个线程的**创建者、数量、循环体、阻塞/唤醒、退出方式**。

### 概览表

| # | 线程 | 数量 | 创建者 | 阻塞机制 | 退出条件 |
|---|------|------|--------|----------|----------|
| ① | config_monitor | 1 | `app_ctrl_init` | `cv_config` 定时等待(2s) | `config_monitor_exit` |
| ② | fd_monitor | 1 | main `pthread_create` | `cv_config` 定时等待(60s) | `fd_monitor_exit` |
| ③ | capture_bus | 唯一流数 | `DecChannel::init` | `gst_bus_timed_pop`(500ms) | `isRunning`=0 / stop |
| — | GStreamer streaming | 由 GStreamer 定 | GStreamer 内部 | appsink 缓冲驱动 | 管道销毁 |
| ④ | display_worker | 显示通道数 | main `pthread_create` | `DispQueue.cv` 等待 | `isRunning`=0 |
| ⑤ | dispatch_worker | 推理通道数 | main `pthread_create` | `result_ready_cv`(100ms) | `g_dispatch_running`=0 |
| ⑥ | infer_worker | Σ每通道 threads | `algorithm_init` | `TaskQueue.cv` 等待 | `running`=0 / reload |
| ⑦ | global_logic | 启用实例数 | `global_logic_start_all` | `usleep(poll)` | `running`=0 |
| ⑧ | upload_worker | 1 | `alarm_uploader_init` | `queue_cv` 等待 | `g_uploader_running`=0 |
| — | main 主线程 | 1 | 进程入口 | GTK 主循环 / `usleep` | 窗口关闭 / 信号 |

---

### ① config_monitor_thread — 配置热重载

**文件**：`core/app_ctrl.cpp` · `config_monitor_thread_func`

**循环体**：
1. `pthread_cond_timedwait(cv_config, 2s)` — 定时唤醒，或被退出信号提前唤醒
2. 比较 `config.json` 的 `mtime`，无变化则继续
3. 检测到变化后**再等一轮**（等文件写入稳定，防止读到半截 JSON）
4. `load_config()` 解析到临时 `AppConfig`
5. **锁外**比对哪些通道模型路径变了 → `algorithm_reload_channel_model()`（耗时操作绝不持锁）
6. **写锁内**仅做 `sync_fields()` 字段级原地同步（不整体赋值，避免 vector 重分配使指针失效）
7. 锁外更新阈值、类别白名单、跟踪器参数；logic 名变化时清空对应通道的 `logic_state`

**设计要点**：耗时的模型重载在锁外执行，写锁只用于微秒级的字段同步，避免阻塞推理线程读配置。

**退出**：`config_monitor_exit=1` + `pthread_cond_broadcast(cv_config)`。

---

### ② fd_monitor_thread — 文件描述符监控

**文件**：`main.cpp` · `fd_monitor_thread_func`

**循环体**：`pthread_cond_timedwait(cv_config, 60s)` → 读 `/proc/self/fd` 统计 fd 数 → `performance_display` 开启时打印 `fd_in_use / soft_limit / hard_limit`。

**存在意义**：RK3588 多路 RTSP 长跑时，GStreamer 信号watch 误用会泄漏 fd（一对 wakeup pipe）。本线程提供运行期可观测性，配合 `capturer` 用主动轮询 bus 而非 signal-watch，从根上杜绝泄漏。

**退出**：`fd_monitor_exit=1` + broadcast。

---

### ③ capture_bus_thread — GStreamer 总线监听 + 重连

**文件**：`capturer/decChannel.cpp` · `busListen`

**数量**：每个**唯一流源**一个（多个逻辑通道共享同一 RTSP 时只建一个，通过 `addTargetChannel` 关联）。

**循环体**：
1. `gst_bus_timed_pop_filtered(500ms)` 主动轮询 bus 消息（**不用** `gst_bus_add_signal_watch`，避免 fd 泄漏）
2. 处理消息：
   - `GST_MESSAGE_ERROR` → 触发重连
   - `GST_MESSAGE_EOS` → 文件循环则重建管道；非循环文件则退出线程
   - 静默断流（文件 3s / 实时流 15s 无新帧）→ 触发重连
3. 重连前调 `analyzer_channel_offline(cid)`（清旧框/旧状态），重连成功后调 `analyzer_channel_online(cid)`

**退出**：`isRunning=0` 或 `mStopRequested=true`，500ms 超时使其能及时响应退出。

---

### — GStreamer streaming 线程 → videoOutHandle（帧入口）

**文件**：`analyzer/frame_inlet.cpp` · `videoOutHandle`（由 appsink `new_sample` 回调驱动，运行在 GStreamer 内部 streaming 线程）

> ⚠️ 这是回调，不是我们 `pthread_create` 的线程；**绝不能在此睡眠**，否则耗尽 `mppvideodec` 的 buffer pool 导致硬件解码死锁。

**每帧固定三步**：
1. **FPS 节流**（phase-offset 错相）：按 `max_fps` 计算下次允许推理时刻 `next_due_us`，并按通道号错开初始相位，避免多路同时触发 NPU 峰值；未到时刻则跳过推理（仅显示）
2. **RGA 转换**：NV12 → BGR 640×640（`convertToYoloInput`），生成 logic/上报所需的底图
3. **分流**：
   - **显示**（每帧都做）：锁外 `memcpy` 到 DispFramePool 的 back 槽 → 持锁整数级 `publish()` → signal `DispQueue.cv`
   - **推理通道**：`algorithm_process_mat` 入 TaskQueue（队满即丢，记 `drop`）
   - **非推理通道**：直接 `process_channel_results`（同步，持 `g_process_mtx`）

**统计**：每 5 秒打印 `recv/throttle/enq/drop/conv` 一行。

---

### ④ display_worker_thread — 异步显示

**文件**：`analyzer/display_pipeline.cpp`

**数量**：每个启用显示的通道一个。

**循环体**：
1. `pthread_cond_wait(DispQueue.cv)` 阻塞至有新帧（零 CPU 占用）
2. 持锁拷贝元数据（6 个整数）+ `swap_front_if_dirty()`（mid↔front 整数交换）
3. **锁外** `commitImgtoDispBufMap`：RGA 缩放 → `render_overlays`（读共享 `last_results`，按 `result_age_ms` 卡尔曼速度外推画框）→ 写 framebuffer

**与推理结果的关系（有意设计）**：显示的是**最新解码帧**，叠加框来自共享 `last_results`（可能旧几帧），用速度外推补偿管线延迟。这是实时预览的合理取舍——见 [§6](#6-帧-结果时序保证)。

**退出**：`isRunning=0`；退出时 main 调 `analyzer_wake_display_threads()` 唤醒阻塞在 cv 上的线程，join 后才销毁队列原语。

---

### ⑤ dispatch_worker_thread — NPU 结果分发

**文件**：`analyzer/result_dispatch.cpp`

**数量**：每个推理通道一个。

**循环体**：
1. `algorithm_wait_result(chnId, 100ms)` 阻塞等 NPU 完成通知（100ms 超时便于退出）
2. `algorithm_take_results()` **原子取出**检测框 + 产生它的 640 输入图 + frame_seq（同一把 `channel_results[i].mtx`）
3. 读最新解码帧信息作兜底
4. `process_channel_results()`（持 `g_process_mtx[i]`，与 videoOutHandle 非推理直通路径互斥）
5. `debug_display` 开时每 2 秒打印帧匹配诊断：`result_seq / input_seq / lag`

**退出**：`g_dispatch_running=0`，靠 100ms 超时跳出 wait。

---

### ⑥ infer_worker_thread — NPU 推理

**文件**：`analyzer/algo_engine.cpp` · `worker_thread_func`

**数量**：所有推理通道的 `threads` 之和（每个模型实例一个 worker）。多通道可共享同一模型实例，用 `model->infer_mtx` 串行化。

**循环体**：
1. `pthread_cond_wait(TaskQueue.cv)` 阻塞等任务
2. 出队 `AlgoTask`（含 frame_seq、640 图、源 DMA-BUF 句柄）
3. 持 `model->infer_mtx`：
   - 优先 **RGA 零拷贝前处理**（`rga_convert_resize_handle` 直接写 RKNN 输入内存）→ `infer_zero_copy`
   - 失败回退 CPU `infer(task.img)`
4. 后处理：置信度过滤 → 类别白名单过滤 → NMS → 上限 20 框
5. 写结果（**关键**）：持 `channel_results[i].mtx`，仅当 `seq > latest_seq` 时原子写入 `data`(框) + `data_frame`(640图) + `latest_seq`
6. signal `result_ready_cv[i]` 通知 dispatch_worker
7. `g_fps[i].tick()`；每 5 秒打印 `wait_q/lock/pre/npu/post/nms/total` 耗时分解

**退出**：`g_algo.running=0` 或 `chn_reload_stop[chnId]=1`（热重载该通道时）。

---

### ⑦ global_logic_thread — 跨路全局逻辑

**文件**：`logic/global_logic.cpp` · `global_logic_thread_func`

**数量**：`config.global_logics` 中 `enable=true` 的实例数，每实例一个独立线程。

**循环体**：
1. `pause_ctrl::wait_if_paused()`
2. 遍历监控的通道，比较 `last_infer_ts`，有新推理则 `app_ctrl_get_results_fresh()` 取快照入 `results_cache`
3. 填充 `GlobalContext`（tick_id、has_new_infer、latest_infer_channel 等）
4. 调用用户 `func(&gctx)`
5. `usleep(poll_ms - elapsed)` —— `poll_interval_ms` 会自动收敛到推理帧间隔的一半

**特点**：与各通道帧管线**完全解耦**，独立节奏轮询；不直接写屏幕（draw_cmds 属各通道 dispatch_worker），只能通过 `get_channel_snapshot` 取数后上报。

**退出**：`t->running=0`。

---

### ⑧ upload_worker_thread — Redis 告警上传

**文件**：`uploader/alarm_uploader.cpp`

**数量**：1（全局唯一消费者）。

**循环体**（生产者-消费者）：
1. `pthread_cond_wait(queue_cv)` 阻塞至队列非空
2. 出队 `AlarmTask` / `DifyTask`
3. server 告警 → `record_alarm_local()` 落盘本地发件箱（带框图.jpg + 原图_raw.jpg + .json，rename 原子提交）；dify → JPEG+Base64 `redis_rpush("dify_queue")`
4. Redis 断连时自动重连（限流 5s 一次）

**生产者**：logic 中调 `alarm_uploader_enqueue()` / `dify_uploader_enqueue()`，**非阻塞**入队，不阻塞推理线程。**地址随消息走（方案2）**：入队函数带一个**本通道**地址参数（`server_url` / `dify_api_url`+`dify_api_key`，取自 `ctx->config`），server 走 `record_alarm_local` 把地址写进发件箱 `.json`、dify 走 `build_and_push_dify` 写进 Redis `dify_queue` 消息体；Python 上报服务按"记录/消息里的地址"转发——所以不同通道可发往不同服务器，C++ 自身不连业务服务器。详见文末「近期架构增强 A」。

**退出**：`g_uploader_running=0` 且队列排空。

---

### — main 主线程

`main.cpp`：完成全部初始化与线程创建后：
- `enable_display=true` → `display()` 进入 GTK 主循环（阻塞至窗口关闭）
- `enable_display=false` → `while(isRunning) usleep(500ms)` 空转

收到 SIGINT/SIGTERM 后驱动逆序退出（见 [§7](#7-生命周期启动与退出)）。

---

## 4. 同步原语清单

| 原语 | 类型 | 保护对象 | 说明 |
|------|------|----------|------|
| `g_pCtrl->mtx` | rwlock | 全局 `config` | 多读单写；config_monitor 写、各线程读 |
| `g_pCtrl->chn_mtx[i]` | mutex | `channels_state[i]` | 每通道独立；logic 写回 / snapshot 读 |
| `g_process_mtx[i]` | mutex | `process_channel_results` 串行 | videoOutHandle 直通 ⇿ dispatch_worker 互斥 |
| `cv_config` (+`cv_config_mtx`) | condvar | 定时唤醒 / 退出 | config_monitor、fd_monitor 共用 |
| `DispQueue[i].mtx` + `cv` | mutex+cv | DispFramePool 槽交换 | videoOutHandle → display_worker |
| `TaskQueue.mtx` + `cv` | mutex+cv | 推理任务队列 | videoOutHandle → infer_worker |
| `channel_results[i].mtx` | mutex | 框+640图+seq 三元组 | **帧-结果原子配对的关键锁** |
| `result_ready_cv[i]` (+mtx) | condvar | 「有新结果」 | infer_worker → dispatch_worker |
| `g_algo.dispatch_mtx` | rwlock | `task_queues` 结构 | 热重载写、process_mat 读 |
| `model->infer_mtx` | mutex | 共享模型实例 | 多 worker 串行推理同一模型 |
| `detect_classes_mtx` | mutex | 类别白名单 | config_monitor 写、worker 读 |
| `g_queue_mtx` + `queue_cv` | mutex+cv | 告警/Dify 队列 | logic → upload_worker |
| `g_redis_mtx` | mutex | Redis 上下文 | upload_worker 串行访问 |
| `g_feed_mtx` | mutex | FPS `next_due_us` | videoOutHandle 自用 |

**锁层级约定**（避免死锁）：耗时操作（模型重载、推理、网络）一律在锁外；`chn_mtx` 只用于微秒级写回；不存在嵌套持有 `mtx` 和 `chn_mtx` 的路径。

---

## 5. 关键数据结构

### APP_CTRL（core/app_ctrl.h）

全局唯一单例，`extern APP_CTRL *g_pCtrl`。含 `config`、`channels_state[N]`、`pDispBuffer`、全部锁、`isRunning`、各线程句柄。

### ChannelState — 三组所有权

| 组 | 字段 | 访问规则 |
|----|------|----------|
| (A) display_worker 独占 | `disp_fps`、`fps_counter` | 仅 display_worker 写 |
| (B) `chn_mtx[i]` 保护 | `last_results`、`last_logic_frame`、`result_frame_seq`、`draw_cmds`、`logic_state`、`roi_*` | 多线程持锁读写 |
| (C) 单线程 | `last_infer_ts_ms`、`input_frame_seq` | videoOutHandle 自用 |

### DispFramePool — 三槽显示帧池（analyzer_internal.h）

三个缓冲槽轮转（back 写 / mid 就绪 / front 读），不变量 `back≠mid≠front`。**3MB 帧拷贝在锁外完成**，持锁临界区只做整数级槽交换（≈20ns）。解决了原先「持锁 memcpy 3MB 阻塞显示线程」的问题。

### channel_results[i]（algo_internal.h）

`data`(检测框) + `data_frame`(640输入图) + `latest_seq`(帧序号) 三者在同一把 mutex 下原子写入/读出——**帧-结果同帧匹配的物理基础**。

### DispTask / DispQueue — 单槽覆盖

显示队列是单槽：新帧到达若上帧未取走则**直接覆盖**，保证屏幕永远显示最新解码帧（流畅性优先于不丢帧）。

---

## 6. 帧-结果时序保证

系统刻意区分两条路径，时序语义不同：

### 路径 A：logic / 上报 —— 严格同帧匹配 ✅

```
infer_worker：持 channel_results[i].mtx 原子写 {框, 640图, seq}
     │
dispatch_worker：algorithm_take_results 持同一锁原子取出三者
     │
invoke_channel_logic：持 chn_mtx[i] 原子写回 last_results + last_logic_frame + result_frame_seq
     │
get_channel_snapshot：持 chn_mtx[i] 一次性读出 frame+results+state
```

**结论**：业务逻辑拿到的 `ctx->frame` 与 `ctx->results`、跨通道 `snapshot.frame` 与 `snapshot.results`，**永远来自同一 frame_seq**，绝不会「帧是旧的、框是另一时刻」。告警上报图与框严格一致。

### 路径 B：屏幕显示 —— 有意不严格匹配 ⚠️

显示线程取**最新解码帧**，叠加的框来自共享 `last_results`（可能旧几帧），靠 `render_overlays` 内的卡尔曼速度外推补偿延迟。这是「显示流畅优先」的有意取舍：

- 优点：画面始终是最新的，不卡顿
- 代价：推理卡顿/断流瞬间，旧框可能短暂滞留（已用 `analyzer_channel_offline` 在断流时清框缓解）
- 不影响路径 A 的上报准确性

---

## 7. 生命周期：启动与退出

### 启动顺序（main.cpp）

```
raise_fd_limit → app_ctrl_init(创建 config_monitor) → gst_init
→ sigaction → dispBufferMap → analyzer_init(创建 infer_worker + global_logic)
→ pthread_create(config_monitor*, fd_monitor)
→ DecChannel::init(创建 capture_bus + GStreamer streaming)
→ pthread_create(display_worker, dispatch_worker)
→ alarm_uploader_init(创建 upload_worker)
→ display() 主循环
```
*注：config_monitor 句柄在 app_ctrl_init 内准备，main 中 pthread_create 启动。

### 退出顺序（逆序 join，先停源头）

```
信号 → isRunning=0 + broadcast cv_config + resume 暂停态
① capturers[i]->stop() + delete   ← 先断数据源头
② analyzer_deinit：stop+join infer_worker、global_logic、trackers
③ join dispatch_worker
④ analyzer_wake_display_threads → join display_worker
⑤ analyzer_destroy_display_queues  ← display 线程退出后才销毁其原语
⑥ alarm_uploader_deinit（停 upload_worker）
⑦ join fd_monitor
⑧ join config_monitor
⑨ app_ctrl_deinit
```

**核心原则**：先停产生数据的线程（采集→推理），再由后向前停消费者，**最后**才销毁队列同步原语——确保没有线程仍阻塞在即将被销毁的 mutex/cv 上。

---

## 8. 硬件与环境约束

### RGA 调度核心（不可修改）

```cpp
// rga_convert.cpp（三处 RGA 调用）— 禁止改动
opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
```

RK3588 有 RGA3-Core0、RGA3-Core1、RGA2 三个核。本程序**只用 RGA3 双核**；误用 RGA2 或第三核会导致硬件崩溃，只能断电恢复。

### 编译与运行

- **开发在 Windows，编译运行在板端**：所有改动以源码为准，复制到 RK3588 编译。
- 板端：`mkdir build && cd build && cmake .. && make -j4`，运行 `./rk3588_yolo assets/config.json`。

### 配置热重载

支持（无需重启）：检测阈值、类别白名单、跟踪器参数、通道 logic 名、模型路径，以及**所有经 `REG_C` 注册的通道字段**（业务逻辑的可调参数、每通道上报地址 `server_url`/`dify_api_url`/`dify_api_key` 等）——`config_monitor` 经 `sync_fields()` 字段级热拷，逻辑下一帧即读到新值。
不支持（需重启）：通道数量、流地址、显示分辨率、NPU 核心数，以及 **ROI**（`roi_zones.json` 仅在启动 `load_roi_zones()` 时加载一次，**改 ROI 必须停止再启动程序**）。

---

## ✚ 近期架构增强（2026-06，对上文的补充，以源码为准）

上文未展开、近期落地的几处变化，写新代码 / 排查问题时需知：

### A. 上报地址「每通道」化（方案2）—— 地址跟着告警走
`alarm_uploader_enqueue` / `dify_uploader_enqueue` 各多了**本通道地址**参数：
```cpp
alarm_uploader_enqueue(img_draw, img_raw, chnId, alarm_type, server_url);
dify_uploader_enqueue(img, prompt, event_id, dify_api_url, dify_api_key);
```
- 地址取自 `ctx->config->server_url` / `dify_api_url` / `dify_api_key`（每通道字段，`config_init.cpp` 用 `REG_C` 注册、随配置热重载；用户在网页「上报配置」节点逐通道填）。
- server 走 `record_alarm_local` 把地址写进本地发件箱 `.json`、dify 走 `build_and_push_dify` 把地址写进 Redis `dify_queue` 消息体（按"方式"分而非按"地址"分）。
- **C++ 不连业务服务器**：真正发到哪台由 Python 上报服务读消息里的地址决定 → 不同通道/程序可发不同服务器，改分发规则只动 Python、不重编译 C++。
- 涉及文件：`uploader/alarm_uploader.{h,cpp}`、`config/config_init.cpp`、`logic/logic_*.cpp`（各 enqueue 调用点，如 logic_server / logic_hook / logic_dify）。

### B. ROI 坐标系与加载（务必遵守，否则判定错位）
- **逻辑里 `ctx->roi`（即 `roi_for_logic`）在模型输入坐标系（640×640）**，与 `ctx->results[].box` 同坐标系；判定用 `roi_contains(ctx, r.box, ROI_ALL)`（内部按模型坐标算），**不要拿源分辨率坐标去比**。
- `roi_zones.json` 现存**归一化坐标 0~1**（占画面比例，与视频源/分辨率解耦）；`load_roi_zones()` 加载时 × 模型尺寸落到 640 空间。旧像素格式自动兼容（坐标 >1 视为像素，运行时按 src→model 缩放）。
- **ROI 只在启动 `load_roi_zones()` 加载一次，不热重载** —— 改 ROI 必须停止再启动。
- 推理通道源分辨率经 `channels_state[i].src_w_now`（`frame_inlet` 每帧写，`result_dispatch` 取）传给 ROI 缩放，确保推理通道 ROI 不偏（修了"推理通道 src_w 停在 0 → ROI 一直框外"的 bug）。

### C. USB 采集分辨率可显式固定（与 fps 解耦）
- `StreamConfig` 加 `usb_width`/`usb_height`（config.json 的 `stream` 子对象）。设了就**固定按它采集**（`capturer/decChannel.cpp`），不再随 `max_fps` 变分辨率/视野——根治"改 fps → USB 视野变 → ROI 偏移"。`0`=自动（按 fps 离散档）。
- ROI 编辑器抓帧也用同一显式分辨率，保证"画的那张图 == 推理跑的那张图"。

---

## 9. 二次开发快速入门（添加业务逻辑）

详见 [logic.md](../../rk3588-src-modules/logic.md)。三步接入一个新的 channel_logic：

1. 新建独立文件 `src/logic/logic_xxx.cpp`（顶部 `#include "logic_common.h"`）实现函数，并在**文件末尾**自注册：
   ```cpp
   static void logic_xxx(ChannelContext *ctx) {
       // ctx->results  — 当前帧检测结果（已跟踪）
       // ctx->frame    — 当前帧 BGR 640×640（与 results 同帧保证）
       // ctx->frame_id — 帧序号
       // draw_rect / draw_text — 叠加自定义图文
       // alarm_uploader_enqueue — 触发告警上报
   }
   REGISTER_LOGIC("logic_xxx", logic_xxx);   // main() 前自动登记到分发表，无需改 channel_logic.cpp
   ```
2. `src/logic` 下的 `.cpp` 由 CMake 自动收集编译，无需手动注册（删除逻辑＝删除该文件）。
3. 在 `config.json` 对应通道设置：
   ```json
   { "logic": "logic_xxx" }
   ```

修改后无需重启——配置热重载会自动切换 logic 并清空跨帧状态。

---

## 10. 项目目录结构

```
rk3588_yolo/
├── src/
│   ├── main.cpp               程序入口，线程创建与生命周期管理
│   ├── system.h               DBG_PRINT 调试宏
│   ├── analyzer/              推理调度核心
│   │   ├── analyzer.h             对外接口声明
│   │   ├── analyzer_internal.h    内部数据结构（DispFramePool 等）
│   │   ├── frame_inlet.cpp        GStreamer 帧入口 + FPS 节流
│   │   ├── rga_convert.cpp        NV12→BGR RGA 硬件转换
│   │   ├── algo_engine.cpp        NPU 任务队列 + infer_worker
│   │   ├── channel_pipeline.cpp   跟踪 + logic 分发
│   │   ├── result_dispatch.cpp    dispatch_worker 线程
│   │   ├── display_pipeline.cpp   display_worker 线程
│   │   └── display_render.cpp     commitImgtoDispBufMap + 叠加渲染
│   ├── capturer/              GStreamer 视频采集
│   │   └── decChannel.cpp         采集器实现（bus 轮询 + 重连）
│   ├── config/                JSON 配置解析与热重载
│   │   ├── config.h / config.cpp  配置结构体 + 解析
│   │   └── config_registry.cpp    注册表 + sync_fields 字段级同步
│   ├── core/                  全局控制块与工具函数
│   │   ├── app_ctrl.h / app_ctrl.cpp  APP_CTRL + 通道状态 + snapshot
│   │   ├── image_utils.*              原始帧 → BGR cv::Mat
│   │   ├── base64_util.*              告警图 Base64 编码
│   │   ├── pause_ctrl.*               空格暂停/继续
│   │   └── constants.h                系统级常量(队列大小等)
│   ├── logic/                 业务逻辑（二次开发在这里）
│   │   ├── channel_logic.cpp      框架核心：ChannelContext 方法 / draw_* / 注册分发表
│   │   ├── logic_common.h         各 logic 共用头集合（逻辑文件一行 #include 它）
│   │   ├── logic_xxx.cpp          各通道逻辑，一个逻辑一个文件（末尾 REGISTER_LOGIC 自注册）
│   │   ├── logic_tools.{h,cpp}    共享状态结构/算法（HookState、占用率计算等）
│   │   └── global_logic.cpp       跨路独立线程轮询逻辑
│   ├── player/                GTK3 显示
│   │   └── display.cpp            窗口创建 + 双缓冲 + render_overlays
│   ├── uploader/              告警异步上报
│   │   └── alarm_uploader.cpp     JPEG 编码 + Redis RPUSH
│   ├── yolo/                  RKNN 推理引擎
│   │   ├── model_base.h           ModelBase 抽象基类
│   │   ├── yolo.cpp               YOLOv5 实现
│   │   ├── yolov8det.cpp          YOLOv8-det 实现
│   │   ├── yolopose.cpp           YOLOv8-pose 实现
│   │   └── yoloseg.cpp            YOLOv5-seg 实现
│   └── third_party/           cJSON、gst_opt、rk_mpi 等
├── assets/
│   ├── config.json            运行配置
│   ├── roi_zones.json         ROI 多边形配置
│   └── *.rknn / labels.txt    模型和标签文件
├── dist/
│   └── services/
│       ├── upload/            Python 告警上报微服务
│       └── model_update/      OTA 模型更新服务
└── CMakeLists.txt
```

---

## 附：线程交互速查图

```
                          ┌─────────────────┐
                          │ config_monitor  │──热重载──▶ algoProcess / trackers
                          └─────────────────┘
  GStreamer streaming
        │ videoOutHandle
        ├──显示──▶ DispQueue ──▶ display_worker ──▶ framebuffer（读 last_results 外推）
        │
        ├──推理──▶ TaskQueue ──▶ infer_worker ──▶ channel_results
        │                                              │ result_ready_cv
        │                                              ▼
        │                                        dispatch_worker
        │                                              │ invoke_channel_logic
        └──非推理──────────────────────────────────────┤（持 g_process_mtx 互斥）
                                                        ▼
                                              last_results / draw_cmds（chn_mtx）
                                                        │
              global_logic ──get_results_fresh──────────┤
                                                        ▼
                                          alarm_uploader_enqueue ──▶ upload_worker ──▶ Redis
```
