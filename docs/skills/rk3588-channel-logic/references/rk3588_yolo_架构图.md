# rk3588_yolo 架构图

> 配套文档：[rk3588_yolo_系统说明文档.md](rk3588_yolo_系统说明文档.md)
>
> 本文件用图说明系统结构与线程拓扑；文字详解见系统说明文档。

---

## 1. 系统组件总览

```
                              ┌──────────────────┐
                              │   config.json    │
                              │   roi_zones.json │
                              └────────┬─────────┘
                                       │ 加载 / 2s 热监控
                                       ▼
   ┌───────────────────────────────────────────────────────────────────────┐
   │                        core/  APP_CTRL（全局控制块）                   │
   │   config · channels_state[N] · pDispBuffer · 锁集合 · isRunning        │
   └───────────────────────────────────────────────────────────────────────┘
            ▲                    ▲                    ▲              ▲
            │ 读写状态            │                    │              │
   ┌────────┴──────┐   ┌─────────┴────────┐   ┌───────┴──────┐  ┌────┴──────┐
   │  capturer/    │   │    analyzer/     │   │   logic/     │  │ uploader/ │
   │  GStreamer    │──▶│  推理调度核心    │──▶│ channel/     │─▶│ Redis异步 │
   │  RTSP/文件/USB│帧 │  (见线程拓扑图)  │   │ global logic │  │ 上报      │
   └───────────────┘   └────────┬─────────┘   └──────────────┘  └───────────┘
                                │                     │
                       ┌────────┴────────┐   ┌────────┴────────┐
                       │     yolo/       │   │    player/      │
                       │  RKNN NPU 推理  │   │  GTK3 + RGA显示 │
                       └─────────────────┘   └─────────────────┘
```

---

## 2. 线程拓扑图（谁创建谁）

程序共 **8 类自建线程** + GStreamer 内部线程 + main 主线程。所有线程在 `main()` 中清晰可循：

```
main 线程
  │
  ├─ app_ctrl_init() ───────────────▶ ① config_monitor_thread   ×1     (配置热重载)
  │
  ├─ pthread_create ────────────────▶ ② fd_monitor_thread       ×1     (fd 用量监控)
  │
  ├─ DecChannel::init() ────────────▶ ③ capture_bus_thread      ×唯一流数 (bus监听+重连)
  │        └─ (GStreamer 内部) ······▶   GStreamer streaming     ×N    (new_sample 回调)
  │
  ├─ pthread_create ────────────────▶ ④ display_worker          ×显示通道数 (RGA缩放+显示)
  │
  ├─ pthread_create ────────────────▶ ⑤ dispatch_worker         ×推理通道数 (取结果+调logic)
  │
  ├─ analyzer_init()
  │     ├─ algorithm_init() ────────▶ ⑥ infer_worker            ×Σ每通道threads (NPU推理)
  │     └─ global_logic_start_all()─▶ ⑦ global_logic            ×启用实例数 (跨路轮询)
  │
  ├─ alarm_uploader_init() ─────────▶ ⑧ upload_worker           ×1     (JPEG编码+Redis)
  │
  └─ display() ─────────────────────▶   主线程进入 GTK 主循环（阻塞至窗口关闭）
```

---

## 3. 运行时数据流（线程间如何传递帧与结果）

```
   GStreamer streaming 线程
   （appsink new_sample 回调，每解码一帧触发）
            │
            ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  videoOutHandle()           [frame_inlet.cpp]                │
   │   1. FPS 节流（phase-offset 错相）→ 决定 will_infer           │
   │   2. RGA 转换 NV12→BGR 640×640                               │
   │   3. 生成单调 frame_seq                                       │
   └───┬─────────────────────┬──────────────────────┬─────────────┘
       │ [显示:每帧]          │ [推理通道]            │ [非推理通道]
       ▼                     ▼                      ▼
 ┌───────────┐      ┌─────────────────┐   ┌──────────────────────────┐
 │ 锁外memcpy│      │algorithm_process│   │ process_channel_results  │
 │ →DispQueue│      │ _mat → TaskQueue│   │ （同步，持 g_process_mtx）│
 │ (单槽覆盖)│      └────────┬────────┘   └────────────┬─────────────┘
 │ signal cv │               │ cv signal               │
 └─────┬─────┘               ▼                         │
       │            ┌──────────────────┐               │
       │            │ ⑥ infer_worker   │               │
       │            │  RGA前处理(零拷贝)│               │
       │            │  model->infer()  │               │
       │            │  过滤/NMS/类别   │               │
       │            │  写 channel_     │               │
       │            │  results[seq]    │               │
       │            │  signal ready_cv │               │
       │            └────────┬─────────┘               │
       │                     ▼                         │
       │            ┌──────────────────────┐           │
       │            │ ⑤ dispatch_worker    │           │
       │            │  wait_result(100ms)  │           │
       │            │  take_results(原子)  │           │
       │            │  process_channel_    │           │
       │            │  results (g_process_ │           │
       │            │  mtx 串行)           │           │
       │            └────────┬─────────────┘           │
       │                     │                         │
       │                     ▼   ◀─────────────────────┘
       │            ┌──────────────────────────────────┐
       │            │ invoke_channel_logic             │
       │            │  fn(&ctx)  ← 用户业务逻辑         │
       │            │  原子写回 last_results /          │
       │            │  last_logic_frame / draw_cmds     │
       │            │  (持 chn_mtx[i])                  │
       │            │  可调 alarm_uploader_enqueue() ───┼──┐
       │            └──────────────────────────────────┘  │
       ▼                                                   │
 ┌──────────────────────────────┐                         │
 │ ④ display_worker             │                         │
 │  wait cv → swap_front        │     ┌───────────────────▼────────┐
 │  commitImgtoDispBufMap:      │     │ ⑧ upload_worker            │
 │   RGA缩放→render_overlays    │     │  wait queue_cv             │
 │   (读共享last_results+卡尔曼  │     │  JPEG编码+base64           │
 │    速度外推)→framebuffer     │     │  Redis RPUSH               │
 └──────────────────────────────┘     └────────────────────────────┘

 ⑦ global_logic（独立轮询，与上面解耦）
     usleep(poll) → app_ctrl_get_results_fresh(ch) → func(&gctx) → 可上报
```

---

## 4. 帧-结果时序匹配（核心正确性保证）

系统有**两条取数路径**，时序语义不同：

```
═══════════════ 路径 A：logic / 上报（严格同帧匹配）═══════════════

 infer_worker                          dispatch_worker / logic
 ──────────                            ──────────────────────
 持 channel_results[i].mtx:            持 channel_results[i].mtx:
   data       = 检测框   ┐ 同一把锁      take_results 一次性原子取出
   data_frame = 640输入图├ 同一seq ───▶  (data, data_frame, seq)
   latest_seq = seq     ┘ 原子写入            │
                                              ▼
                                       invoke_channel_logic
                                       持 chn_mtx[i] 原子写回:
                                         last_results / last_logic_frame
                                         / result_frame_seq （同帧）
                                              │
                                              ▼
                                       get_channel_snapshot
                                       持 chn_mtx[i] 一次性读出
                                       frame + results + state（必同帧）

 ✅ 保证：frame 与 results 永远来自同一 frame_seq，绝不错位。


═══════════════ 路径 B：屏幕实时显示（有意不严格匹配）═══════════════

 videoOutHandle                        display_worker
 ──────────────                        ──────────────
 最新解码帧 ──memcpy──▶ DispQueue ──▶  取最新帧
 (单槽覆盖，永远最新)                   commitImgtoDispBufMap:
                                         读共享 last_results（可能旧几帧）
                                         按 result_age_ms 卡尔曼速度外推
                                         绘制框 → 平滑预览

 ⚠️ 显示帧是最新的，叠加框可能旧几帧，靠速度外推补偿。
    这是「显示流畅优先」的有意取舍，不影响路径 A 的上报准确性。
```

---

## 5. 同步原语全景

```
 锁 / 条件变量              保护对象                       生产者 → 消费者
 ───────────────────────────────────────────────────────────────────────────
 g_pCtrl->mtx (rwlock)     config 全局配置                config_monitor 写 / 各线程读
 g_pCtrl->chn_mtx[i]       channels_state[i]              logic 写 / snapshot 读
 g_process_mtx[i]          process_channel_results 串行   videoOutHandle ⇿ dispatch_worker
 cv_config (+mtx)          定时唤醒 / 退出信号            main 通知 config/fd_monitor
 ───────────────────────────────────────────────────────────────────────────
 DispQueue[i].mtx + cv     显示帧槽交换 (DispFramePool)   videoOutHandle → display_worker
 TaskQueue.mtx + cv        推理任务队列                   videoOutHandle → infer_worker
 channel_results[i].mtx    检测框+输入图+seq 原子三元组   infer_worker → dispatch_worker
 result_ready_cv[i] (+mtx) 「有新结果」通知               infer_worker → dispatch_worker
 g_algo.dispatch_mtx(rw)   task_queues 结构(热重载保护)   reload 写 / process_mat 读
 model->infer_mtx          共享模型实例串行推理           多个 infer_worker 互斥
 detect_classes_mtx        类别白名单                     config_monitor 写 / worker 读
 ───────────────────────────────────────────────────────────────────────────
 g_queue_mtx + queue_cv    告警/Dify 任务队列             logic → upload_worker
 g_redis_mtx               Redis 连接上下文               upload_worker 独占串行
 g_feed_mtx                FPS 节流 next_due_us           videoOutHandle 自用
```

---

## 6. 三槽显示帧池（DispFramePool）— 锁外拷贝设计

```
   三个槽轮转，角色循环：back（写）/ mid（就绪）/ front（读）
   不变量：back_idx ≠ mid_idx ≠ front_idx 始终成立

   生产者 videoOutHandle                消费者 display_worker
   ────────────────────                ───────────────────
   back_buf(size)   ← 取写指针(无锁)
   memcpy 3MB       ← 全程无锁 ✅
   ┌─ 持 DispQueue.mtx                  ┌─ 持 DispQueue.mtx
   │   publish():back↔mid (≈20ns)      │   swap_front_if_dirty:mid↔front(≈10ns)
   └─ 释放 + signal cv                  └─ 释放
                                        front_buf() ← 读指针(无锁) ✅
                                        commitImgtoDispBufMap(无锁缩放渲染)

   ► 3MB memcpy 从锁内移到锁外，临界区只剩整数级槽交换。
```

---

## 7. 启动 / 退出时序

```
   ═══ 启动顺序 ═══                      ═══ 退出顺序（逆序 join）═══

   raise_fd_limit                        信号→isRunning=0 + broadcast cv
   app_ctrl_init ─▶ config_monitor       resume 暂停态
   gst_init                              ↓
   sigaction (SIGINT/TERM/USR1/PIPE)     ① 停采集器 capture_bus（先断源头）
   dispBufferMap (显示缓冲)              ② analyzer_deinit:
   analyzer_init                            stop infer_worker → join
     ├ algorithm_init ─▶ infer_worker       stop global_logic → join
     └ global_logic_start_all ─▶ global   ③ join dispatch_worker
   pthread_create config_monitor         ④ 唤醒+join display_worker
   pthread_create fd_monitor             ⑤ 销毁 display 队列原语
   DecChannel::init ─▶ capture_bus       ⑥ alarm_uploader_deinit (停upload)
   pthread_create display_worker         ⑦ join fd_monitor
   pthread_create dispatch_worker        ⑧ join config_monitor
   alarm_uploader_init ─▶ upload_worker  ⑨ app_ctrl_deinit
   display() 主循环阻塞

   ► 退出核心原则：先停数据源头（采集），再由后向前停消费者，
     最后才销毁各队列的同步原语（确保无线程仍阻塞在其上）。
```

---

## 8. 一条帧的完整旅程（推理通道，端到端）

```
 [GStreamer解码] NV12 帧
      │ new_sample 回调（streaming 线程）
      ▼
 [videoOutHandle] FPS节流命中 → RGA转BGR640 → frame_seq=K
      │ algorithm_process_mat
      ▼
 [TaskQueue] 入队（满则丢，记 drop）
      │ cv signal
      ▼
 [infer_worker] RGA前处理→NPU infer→过滤/NMS→写 channel_results[seq=K]
      │ result_ready_cv signal
      ▼
 [dispatch_worker] take_results 原子取 (框+640图+K)
      │ process_channel_results（持 g_process_mtx）
      ▼
 [invoke_channel_logic] tracker填track_id → fn(&ctx) 业务判断
      │ 持 chn_mtx 原子写回 last_results(seq=K) + draw_cmds
      ├──────────────▶ [可选] alarm_uploader_enqueue → upload_worker → Redis
      ▼
 （与此并行）最新解码帧 K+m 经 DispQueue → display_worker
      → 读 last_results(seq=K) 卡尔曼外推 → framebuffer → 屏幕
```

---

## 9. 近期增强补充（2026-06，详见系统说明文档「近期架构增强」）

```
 上报（方案2：地址跟着告警走，C++ 不连业务服务器）
   logic: enqueue(..., ctx->config->server_url / dify_api_url+key)   ← 每通道地址(config.json)
        → build_and_push_* 把地址写进消息 → redis_rpush(server_queue/dify_queue)（一个队列，消息自带地址）
        → Python 上报服务 BLPOP → 按"消息里的地址"POST（不同通道发不同服务器）

 ROI（坐标系务必一致，否则判定错位）
   roi_zones.json(归一化0~1) ─load_roi_zones(仅启动一次)─▶ ×模型尺寸 ─▶ roi_for_logic(模型640空间)
                                                                    │ 与 ctx->results[].box 同坐标系
   ctx->roi ◀───────────────────────────────────────────────────────┘  判定: pointPolygonTest(*ctx->roi, box_center)
   ⚠ 改 ROI 不热重载 → 必须停止再启动；USB 用 stream.usb_width/height 固定采集分辨率(与 fps 解耦)防偏移
```
