# 引擎架构

## 启动与退出

`main.cpp` 支持三个只读命令：`--list-logics`、`--list-global-logics`、
`--validate-config FILE`。正常启动参数是配置路径；未传参数时当前源码默认
`./assets/config_sop.json`，但当前仓库没有 `logic_path_sop` C++ 模块，因此生产/验收必须显式传入
已校验的实际配置，不能依赖默认文件名代表功能完整。

正常启动大致顺序：配置和 `APP_CTRL` → GStreamer → 显示缓冲 → pipeline/inference → logic control
Unix socket → capturer → display/dispatch workers → RTSP → config/fd monitors → 主循环。全局 logic 在
pipeline 初始化过程中启动；图片/录像 worker 按需创建。

退出时先停止 RTSP 和接收新控制，请求配置/管线退出并 join 可能触发 logic 的线程，随后排空事件
持久化队列、停止录像器、销毁队列和控制块。新线程必须纳入这一所有权顺序，不能依赖进程强退。

## 当前线程模型

| 线程 | 数量/创建位置 | 责任 |
|---|---|---|
| config monitor | 1，main | 250 ms 检查；同一 mtime 连续观察两次后热更 |
| fd monitor | 1，main | 每 60 秒按性能开关打印 fd 使用量 |
| capture bus | 每独立源，capturer | GStreamer bus、断流与重连；相同 type/location 可共享源 |
| display worker | HDMI 或 RTSP 开启时每有效通道一个，main | 最新帧合成、叠加、显示/RTSP 输出 |
| dispatch worker | 每推理通道，main | 等待 NPU 结果并进入结果处理 |
| infer worker | inference 内部 | RKNN 推理任务 |
| global logic | 每启用实例 | 独立周期聚合 |
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
5. 非推理通道在节流命中时同步进入 `process_channel_results(..., nullptr, ...)`；
6. 有 HDMI 或活跃 RTSP 客户端且预览令牌命中时，把源帧放入单槽覆盖显示队列；
7. 最后，节流命中的推理通道才把源帧送入 NPU TaskQueue。

推理完成后，dispatch worker 原子取得同一结果版本的 `LazyVideoFrame + results + frame id/time`，再
调用 `process_channel_results()`。它先运行每通道 tracker，再构造 `ChannelContext`。调用
`model_frame()`/`source_frame()` 时才产生 BGR；logic 完全不取像素就没有这一步开销。

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
FPS、logic 名和不可变 outputs。全局实例每 tick 分别原子读取各通道，因此每一路内部一致，但多路不
保证同一采集时刻。

带图 `ChannelFrameSnapshot` 深拷贝模型帧、results、ROI 和 draw commands。全局的 exact snapshot
接口会核对 publication seq，通道在抓图前已进入下一版时返回 false，不拼接不同版本。

## 断流和重连

`pipeline_channel_offline()` 标记离线并重置节流和 tracker；在线恢复会清空旧 results/draws，避免旧框
冻结到新画面。dispatch 还会拒绝离线期间或重连前产生的迟到 NPU 结果。

业务层必须把断流当成正常状态：全局 ready inputs 会排除离线/过期快照，通道逻辑不应假定每次都能
取得像素。
