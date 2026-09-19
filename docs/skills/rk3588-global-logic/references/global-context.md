# GlobalContext API

本文对应当前 公开的 `rkvision/global_context.h`、`rkvision/snapshots.h` 和引擎实现 `logic/core/global_logic.cpp`。

## Tick 和实例字段

| 字段 | 语义 |
|---|---|
| `config` | 本实例稳定的只读 `GlobalLogicConfig*` |
| `timestamp_ms` | 当前 tick 单调毫秒 |
| `unix_ms` | 当前 tick epoch 毫秒 |
| `dt_ms` | 与上一 tick 的实际间隔；首 tick 为 0 |
| `tick_id` | 从 0 开始的实例内 tick 序号 |
| `effective_poll_interval_ms` | 无通道更新时采用的兜底周期，至少 10 ms；不是固定回调周期 |
| `runtime_generation` | 采样时不可变运行配置的 generation |
| `state` | 每个 `instance_id` 一份的 `shared_ptr<void>*` |
| `logic_parameters` | 已按全局模块 Schema 解析的参数 |

参数读取方法与通道一致：`has_param()`、`param_float/int/bool/string/json()`。

## 推荐业务输入 `ChannelInput`

`gctx->inputs()` 返回本 tick 固定的有效输入列表。偏 C 写法可以改用 `input_count()` 获取数量，
再通过 `input_at(index)` 取得只读 `ChannelInput*`，从而使用普通下标 `for` 循环。每个
`ChannelInput` 提供：

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
- age 不超过 `max(2000 ms, 3 × effective_poll_interval_ms)`，其中后者是兜底周期。

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
`for_each_updated_channel()`。`missed_revisions > 0` 表示两次实际分发之间出现多个发布版本，但快照只保留
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

每个启用实例启动一个 pthread，调度同时包含两条路径：

1. 任一通道提交新业务状态时递增全局发布序号并广播条件变量，全局实例立即醒来；
2. 没有新发布时，最多等待 `effective_poll_interval_ms` 后兜底运行一次，使超时、复位和周期任务继续推进。

每次分发的顺序是：采样通道 → 构造 ready inputs/updates → 处理排队 Action → 调用全局 logic →
等待新发布或兜底超时。调度器在分发之间保留至少 5 ms，避免高帧率、多通道同时发布时持续抢占
CPU。线程在采样前记录全局发布序号；如果取快照或执行回调期间又有通道发布，后续等待会立即
返回，不会因错过条件变量通知而延迟到兜底周期。

这意味着 `dt_ms` 是两次真实回调的间隔，可能远小于或偶尔大于兜底周期。不要用 `tick_id`、
调用次数或 `poll_interval_ms` 计时；连续多次发布也可能被合并为一次最新状态回调。回调超时不会
并发重入同一实例，而会直接推迟后续处理，因此业务代码必须有限时且不得阻塞联网。

热重载按 `instance_id` 精确替换变化实例。状态保留条件是 logic、channels 不变且参数变更策略允许
保留；report policy、poll 等其他字段即使变化也会重建实例，但在上述条件满足时状态仍可保留。
