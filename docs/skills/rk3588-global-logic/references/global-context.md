# GlobalContext API

本文对应当前 `logic/core/global_logic.h/.cpp` 和 `runtime/app_ctrl.h`。

## Tick 和实例字段

| 字段 | 语义 |
|---|---|
| `config` | 本实例稳定的只读 `GlobalLogicConfig*` |
| `timestamp_ms` | 当前 tick 单调毫秒 |
| `unix_ms` | 当前 tick epoch 毫秒 |
| `dt_ms` | 与上一 tick 的实际间隔；首 tick 为 0 |
| `tick_id` | 从 0 开始的实例内 tick 序号 |
| `effective_poll_interval_ms` | 调度器实际使用的周期，至少 10 ms |
| `runtime_generation` | 采样时不可变运行配置的 generation |
| `state` | 每个 `instance_id` 一份的 `shared_ptr<void>*` |
| `logic_parameters` | 已按全局模块 Schema 解析的参数 |

参数读取方法与通道一致：`has_param()`、`param_float/int/bool/string/json()`。

## 推荐业务输入 `ChannelInput`

`gctx->inputs()` 返回本 tick 固定的有效输入列表。每个 `ChannelInput` 提供：

- 身份/帧：`channel_id()`、`frame_id()`；
- 尺寸/运行：`src_width()`、`src_height()`、`infer_enabled()`、`logic_name()`；
- 严格读取：`has()`、`read_string/number/int/bool/json()`；
- 带默认值读取：`get_string/number/int/bool/json()`。

严格读取在 key 缺失或类型不完全匹配时返回 false；合法 0、false、空字符串仍返回 true。Input 和
其中借用的数据只在本次全局回调内有效，需要跨 tick 保存时复制标量或字符串。

`input(configured_id)` 从 ready inputs 按配置 ID 查找。不要把通道 ID 当作 vector 下标。

## 框架过滤

调度器每 tick 获取所有应用通道的轻量快照，然后选择配置 `channels`（非空）或所有通道（空），
再调用 `ChannelLogicSnapshot::readable(timeout)`。只有以下条件全部满足才进入 `inputs()`：

- `has_publication == true`；
- `online_state == CH_ONLINE`；
- `publication_age_ms >= 0`；
- age 不超过 `max(2000 ms, 3 × effective_poll_interval_ms)`。

这套过滤是运行时健康策略。若业务需要比自动阈值更严格的同步或新鲜度，使用下面的原始快照并
显式判断。

## 更新版本

`updated_channels` 记录上个 tick 到当前 tick 的版本变化。`ChannelUpdate` 字段：

- `channel_id`；
- `initial_snapshot`；
- `previous_publication_seq`、`publication_seq`；
- `revision_count`、`missed_revisions`；
- `published_steady_ms`。

可用 `has_updates()`、`channel_update(id)`、`channel_updated(id)`、`latest_update()` 和
`for_each_updated_channel()`。`missed_revisions > 0` 表示轮询期间出现多个发布版本，但快照只保留
最新状态；不能把瞬时 outputs 当作无损事件队列。

不要对所有全局规则无条件 `if (!has_updates()) return`。超时、断流复位、周期事件仍需在没有新
版本的 tick 运行。

## 高级原始快照

`channel_snapshots` 包含应用通道的本 tick 轻量快照。可通过以下方法读取：

- `channel_count()`、`channel_at(index)`；
- `channel(configured_id)`、`contains_channel(id)`；
- `connected_channel_count()`、`connected_channel_at(index)`、`is_connected_channel(id)`；
- `for_each_channel()`、`for_each_connected_channel()`。

`ChannelLogicSnapshot` 除 outputs 外还包含 publication/frame 版本和时间、配置 generation、源尺寸、
推理/显示 FPS、在线状态、logic 名等。各通道分别原子采样，但不保证同一采集时刻；同步业务应
比较 `frame_steady_ms` 并定义允许偏差。

`get_channel_frame_snapshot(id, &out)` 深拷贝同帧图像、results、ROI 和绘制指令；若该通道已经
更新到与本 tick 不同的 publication 版本则返回 false，避免混用版本。这是昂贵接口，只在确需
图像/检测明细时使用。

## 调度与状态

每个启用实例启动一个 pthread。每 tick 的顺序是：采样通道 → 构造 ready inputs/updates → 处理
排队 Action → 调用全局 logic → 等待剩余周期。回调超时不会并发重入同实例，而会直接拉长实际
tick 间隔，因此业务代码必须有限时且不得阻塞联网。

热重载按 `instance_id` 精确替换变化实例。状态保留条件是 logic、channels 不变且参数变更策略允许
保留；report policy、poll 等其他字段即使变化也会重建实例，但在上述条件满足时状态仍可保留。
