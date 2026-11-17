# ChannelContext API

本文只描述当前 `engine/include/channel.h` 暴露的通道接口。修改公共头文件后，
必须同步复核本文。

## 目录

- [身份、帧和时间](#身份帧和时间)
- [推理结果](#推理结果)
- [参数和配置](#参数和配置)
- [类型化 outputs](#类型化-outputs)
- [ROI](#roi)
- [绘制和显示底图](#绘制和显示底图)
- [状态和生命周期](#状态和生命周期)
- [跨通道快照](#跨通道快照)

## 身份、帧和时间

| 成员/方法 | 当前语义 |
|---|---|
| `chnId` | `config.channels[].id`，不是数组下标 |
| `src_width` / `src_height` | 解码源真实尺寸；首帧前可能为 0 |
| `frame_id` | 当前业务帧号 |
| `timestamp_ms` | 近似开机后的单调时间，适合计算间隔 |
| `unix_ms` | 当前业务帧进入管线时的 Unix epoch 毫秒 |
| `dt_ms` | 当前业务帧与上一业务帧的间隔 |
| `time_str()` | 按本地时区返回 `YYYY-MM-DD HH:MM:SS` |
| `infer_enabled` | 当前通道是否开启推理 |
| `inference_valid` | 当前批次是否有有效推理结果；多模型时不等于每个子模型都成功 |
| `infer_fps` / `disp_fps` | 当前推理/显示速率 |

### 两种帧接口

- `business_frame()`：固定 640×640 业务 BGR 图，坐标与 `results[].box` 和 ROI 完全一致；与实际 NPU 输入尺寸独立。
- `source_frame()`：源分辨率 BGR 图，尺寸对应 `src_width × src_height`。

两者都惰性转换：本帧第一次调用时生成，之后复用同一不可变缓存；取帧失败返回 `nullptr`。
返回指针只在本次 callback 生命周期内有效，不能修改或跨帧保存。需要修改或异步持有时显式
`clone()`。不要把源分辨率坐标直接用于模型帧上的检测框或 ROI。

## 推理结果

`results` 是当前帧的 `std::vector<AlgoResult>*`。常用 `AlgoResult` 字段如下，完整定义以 `engine/include/types.h` 为准：

| 字段 | 含义 |
|---|---|
| `box` | 固定 640×640 业务坐标系检测框 |
| `label`, `class_id`, `score` | 标签、类别 ID、置信度 |
| `track_id` | 跟踪 ID |
| `chn_id`, `frame_id`, `timestamp_ms` | 结果来源与时间 |
| `model_id`, `model_type`, `model_index` | 模型身份和通道内序号 |
| `box_color` | 框颜色 |
| `keypoints`, `keypoint_scores`, `pose_schema` | 姿态关键点、分数和稳定的关键点语义 |
| `text_result` | 文本类结果 |
| `boxMask` | 整帧类别分割掩码，不是每个目标的独立实例掩码 |
| `has_obb`, `obb_corners` | 旋转框标识和业务坐标系四角 |
| `depth_map`, `depth_kind` | 深度平面和米制/相对逆深度语义 |

跟踪在 logic 之前执行，因此 logic 看到的是已更新跟踪字段的结果。
使用 `TargetQuery` 指定标签、ROI 和阈值，调用 `ctx->query_targets(query)`；
先检查返回状态，再读取 `count` 和 `best`。标签需与模型 labels 完全一致。

组合标签、模型、置信度和 ROI 条件时，优先使用 `query_targets()`，见
[通用目标查询](../../../sdk/target-query.md)。该接口同时返回有效性、数量和最佳目标。
读取整帧载荷使用 `segmentation_result(model_id)`、`depth_result(model_id)`；业务推理无效时返回空指针，
相对逆深度不能当作米制距离。完整语义见[模型结果契约](../../../reference/model-result-contract.md)。

## 参数和配置

`config` 是只读 `ChannelConfig*`。模块专有参数通过以下方法读取：

- `has_param(key)`；
- `param_float(key)`、`param_int(key)`、`param_bool(key)`；
- `param_string(key)`；
- `param_json(key)`，用于 Schema 类型为 `array` 或 `object` 的值。

启动或热重载时，框架按该模块 `logic.json.parameters` 校验并补默认值。参数 key 和类型不应在
C++ 与 manifest 中重复定义成不同契约。

## 类型化 outputs

框架为每个业务帧创建新的输出集合，logic 通过以下方法发布，不直接访问内部输出绑定：

- `publish_string(key, value)`；
- `publish_number(key, value)`；
- `publish_int(key, value)`；
- `publish_bool(key, value)`；
- `publish_json(key, json)`。

发布的 key/type 必须在 `logic.json.outputs[]` 中以 `string`、`number`、`integer`、`boolean` 或
`json` 声明。logic 未在某帧重新发布的 key 不会自动沿用上一帧；全局逻辑必须把“缺失”和合法的
`0`、`false`、空字符串区分开。

## ROI

`rois` 中的多边形已经缩放到固定 640×640 业务坐标系。可用接口：

- `roi_count()`、`roi_at(i)`、`roi_polygon_at(i)`、`roi_by_name(name)`；
- `roi_find(ctx, name)` 返回区域序号，找不到为 `ROI_NONE`；
- `roi_contains(ctx, box, idx)` 只判断框中心的几何归属；
- `query_targets(query)` 统一处理标签、得分、模型、ROI 筛选和计数。

`ROI_FRAME` 忽略区域，`ROI_ALL` 为区域并集（无区域时整帧），非负序号选择指定区域。
目标查询遇到无效区域会返回错误状态；业务若要求“必须画有效区域”应检查区域和返回状态。

## 绘制和显示底图

`draw_rect/circle/line/text/polyline/poly_filled` 写入当前帧的延迟绘制队列。Target 位掩码是：

| Target | 值 | 出口 |
|---|---:|---|
| `DISPLAY` | 1 | 实时显示 |
| `IMAGE` | 2 | 事件图片 |
| `VIDEO` | 4 | 事件视频 |
| `MEDIA` | 6 | 图片和视频 |
| `ALL` | 7 | 三种出口 |

`replace_display_frame(frame)` 接受 `CV_8UC1/3/4` 的任意尺寸 Mat；模型帧可取得时，
`display_canvas()` 首次调用会克隆一份可写的 640×640 业务 BGR 画布，取帧失败时返回空 Mat，调用方应
检查 `empty()`。两者都只改变当前显示底图，不改变推理帧或事件媒体源；系统标注和 draw 指令仍会
在显示出口叠加。

## 状态和生命周期

`get_state<T>()` 返回每个逻辑实例独立的状态，首次构造、后续复用：

```cpp
auto *state = ctx->get_state<MyState>();
if (!state) return;
```

构造参数可通过 `get_state<MyState>(args...)` 传入；同一个模块的普通回调和 Action 使用同一个类型。
原始 `state` 的类型仍为 `std::shared_ptr<void>*`，已有模块可以继续使用。

不要缓存状态借用指针、`ctx`、`results`、`rois` 或帧指针。绘图绑定是框架私有字段，业务通过绘图函数提交指令。参数热重载是否保留状态取决于变更参数
的 `x-hot-reload` 策略；切换 logic 会建立与新模块匹配的状态。

## 跨通道快照

`get_channel_frame_snapshot(configuredId, &out)` 会在目标通道锁内原子取得帧、results、outputs、
绘制和发布元信息，并在返回前释放锁；带图快照会深拷贝图像，属于高级且更昂贵的接口。
通常的跨通道业务应由通道发布 outputs，再交给全局 logic 聚合。
