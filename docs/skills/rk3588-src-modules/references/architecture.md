# 引擎架构

## 启动与退出

`engine/src/application/main.cpp` 提供不启动视频管线的命令：`--list-logics`、
`--list-global-logics`、`--validate-config FILE` 和 `--watchdog-protocol`。
`--inspect-model FILE` 会加载 RKNN 模型执行元数据检查，不能归为不访问设备的静态命令。
正常启动参数是配置路径；未传参数时默认读取 `./assets/config.json`。

`--watchdog-protocol` 输出服务心跳协议版本，不初始化设备。应用退出重启和业务服务心跳默认关闭，
设备级保护关注操作系统、整机和网络；用户可在“程序故障恢复”中另行开启应用保护。启用应用服务看门狗时才增加独立的
`service_watchdog` 线程：主循环（HDMI 模式由 GTK timer）更新进度，在途推理/业务处理用
`Progress/Scope` 标注执行期限；空闲和暂停不标记为挂起工作。超时后停止服务心跳，由 systemd
终止并重启应用。启动/热加载宽限 300 秒，普通在途操作 120 秒，详见[恢复说明](../../../operations/unattended-recovery.md)。

正常启动大致顺序：配置和 `APP_CTRL` → GStreamer → 显示缓冲 → pipeline/inference → logic control
Unix socket → capturer → display/dispatch workers → RTSP → config/fd monitors → 主循环。全局 logic 在
pipeline 初始化过程中启动；图片/录像 worker 按需创建。

退出由 `main.cpp` 统一编排，当前顺序是：

1. 停止 NPU 排队提交、请求应用和服务心跳退出、唤醒暂停线程，关闭控制接口；
2. 等待配置和 fd 监控线程结束，防止退出中途再重建采集器或模型；
3. 停止 RTSP，通知管线停止，再停止并销毁采集器；
4. join 传统 Logic、结果分发和显示线程，停止管线内部推理/全局业务；
5. 释放业务状态和 GPIO，排空事件持久化队列，再等待录像任务结束；
6. 释放显示队列、显示缓冲、控制块、看门狗与 NPU 占用锁，写入退出结果。

Web 还会核对匹配该次进程的退出状态，不能仅凭 PID 消失认定完整清理。
持久化错误会影响退出码与清理确认；强制 `SIGKILL` 或断电无法执行该收尾流程。
具体协议和操作语义见[程序故障恢复](../../../operations/application-recovery.md)。

## 当前线程模型

| 线程 | 数量/创建位置 | 责任 |
|---|---|---|
| config monitor | 1，main | 250 ms 检查；同一 mtime 连续观察两次后热更 |
| fd monitor | 1，main | 每 60 秒按性能开关打印 fd 使用量 |
| capture bus | 每独立源，capturer | GStreamer bus、断流与重连；相同 type/location 可共享源 |
| display worker | HDMI 或 RTSP 开启时每有效通道一个，main | 最新帧合成、叠加、显示/RTSP 输出 |
| dispatch worker | 每推理通道，main | 等待 NPU 结果并进入结果处理 |
| traditional logic worker | 每有效通道，main | 从单槽最新帧队列取任务；推理关闭时执行业务，推理开启时保持等待 |
| infer worker | inference 内部 | RKNN 推理任务 |
| global logic | 每启用实例 | 通道发布唤醒，poll_interval_ms 作为无更新兜底 |
| logic control | 1 个 std::thread | Unix socket 接收 Action |
| event image/persistence | 按需 | 本地状态和图片生成 |
| event video | 首次收到启用录像通道的有效源帧，或首次触发录像任务时按需创建 | 环形缓冲和 MP4 编码 |

## 帧管线

`pipeline_submit_frame()` 是 appsink 唯一入口：

1. 取得 steady/unix 双时钟和当前不可变通道配置；
2. 若事件视频启用，先按录像自身 FPS 把命中源帧复制到 recorder 队列；这条路径独立于后面的业务
   `max_fps` 节流；
3. 结合配置与运行时 `infer_toggle` 计算本帧是否启用推理，再按通道 `max_fps` 做相位错开的业务节流；
4. 递增 input seq、更新源尺寸，并为节流命中的非推理帧准备惰性帧；
5. 非推理通道在节流命中时把帧发布给单槽最新帧队列，由独立 `pipeline_logic_worker` 调用 `process_channel_results(..., nullptr, ...)`，不在 appsink 中执行业务；
6. 有 HDMI 或活跃 RTSP 客户端且预览令牌命中时，把源帧放入单槽覆盖显示队列；
7. 最后，节流命中的推理通道才把源帧送入 NPU TaskQueue。

推理完成后，dispatch worker 原子取得同一结果版本的 `LazyVideoFrame + results + frame id/time`，再
调用 `process_channel_results()`。它先运行每通道 tracker，再构造 `ChannelContext`。调用
`business_frame()`/`model_frame()`/`source_frame()` 时才产生 BGR；logic 完全不取像素就没有这一步开销。
`business_frame()` 和历史名称 `model_frame()` 共用固定 640×640 业务缓存；`source_frame()` 是原图。
NPU 输入尺寸来自每个模型，始终从原始缓冲生成；结果在进入跟踪、业务和绘图前映射到固定业务画布。
业务尺寸由 `coordinates.h` 定义，不随模型初始化或热切换变化。

## 同步与发布

- `APP_CTRL::mtx` rwlock 保护可变配置对象；业务帧通常读原子发布的不可变运行快照。
- `chn_mtx[id]` 保护 `ChannelState`、在线状态和发布槽。
- `g_process_mtx[id]` 保证同通道业务处理不并发，并与热更新切换 state/tracker 协调。
- 配置热更新先构造完整 `AppRuntimeSnapshot`，再以 `shared_ptr` 原子替换；callback 持有旧/新某一整代。
- logic 在 `chn_mtx` 外执行，提交时把 lazy frame、results、outputs、draw commands、state 和显示底图
  写回，再增加 publication seq。

未配置 logic 或注册 ID 找不到时，框架仍提交当帧 frame/results，但清空业务 outputs/state/draw。
非推理通道的 results 是空 vector，帧 ID 是 logic 业务序号。

## 通道快照与全局逻辑

轻量 `ChannelLogicSnapshot` 不复制图像，包含发布版本、帧时间、配置 generation、在线状态、尺寸、
FPS、logic 名和不可变 outputs。通道提交新版本时递增 publication seq 并立即唤醒全局实例；没有
新发布时，全局实例按 `poll_interval_ms` 兜底运行。每次分发分别原子读取各通道，因此每一路内部
一致，但多路不保证同一采集时刻。快速连续发布可以合并为一次最新状态读取，而不是无损帧队列。

轻量快照同时保留该 publication 的不可变媒体引用，不转换或复制像素。带图
`ChannelFrameSnapshot` 从这份引用深拷贝模型帧、results、ROI 和 draw commands；
全局抓图使用 tick 采样的版本，通道后续发布不会替换它或使抓图失败。
全局 tick 结束释放媒体引用，图片任务只持有复制后的 CPU 图片。异步 DMA-BUF 源同时持有
`GstBuffer`，避免解码池提前复用。预览、上报图片和录像叠加均按检测结果的实际坐标绘制框。

## 断流和重连

`pipeline_channel_offline()` 标记离线并重置节流和 tracker；在线恢复会清空旧 results/draws，避免旧框
冻结到新画面。dispatch 还会拒绝离线期间或重连前产生的迟到 NPU 结果。

业务层必须把断流当成正常状态：全局 ready inputs 会排除离线/过期快照，通道逻辑不应假定每次都能
取得像素。
