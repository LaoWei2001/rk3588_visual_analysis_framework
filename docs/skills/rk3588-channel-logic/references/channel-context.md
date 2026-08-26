# ChannelContext API

本文只描述当前 `vision_analysis/src/logic/core/channel_logic.h` 暴露的通道接口。修改公共头文件后，
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
| `time_hms()` | 按本地时区返回 `HH:MM:SS` |
| `time_str()` | 按本地时区返回 `YYYY-MM-DD HH:MM:SS` |
| `datetime()` | 返回拆分后的 `FrameTime` |
| `infer_enabled` | 当前通道是否开启推理 |
| `infer_fps` / `disp_fps` | 当前推理/显示速率 |

### 两种帧接口

- `model_frame()`：模型输入尺寸的 BGR 图，坐标与 `results[].box` 和 ROI 完全一致。
- `source_frame()`：源分辨率 BGR 图，尺寸对应 `src_width × src_height`。

两者都惰性转换：本帧第一次调用时生成，之后复用同一不可变缓存；取帧失败返回 `nullptr`。
返回指针只在本次 callback 生命周期内有效，不能修改或跨帧保存。需要修改或异步持有时显式
`clone()`。不要把源分辨率坐标直接用于模型帧上的检测框或 ROI。

## 推理结果

`results` 是当前帧的 `std::vector<AlgoResult>*`。当前 `AlgoResult` 字段如下：

| 字段 | 含义 |
|---|---|
| `box` | 模型输入坐标系检测框 |
| `label`, `class_id`, `score` | 标签、类别 ID、置信度 |
| `track_id`, `track_hits`, `vx`, `vy` | 跟踪 ID、命中次数和速度 |
| `chn_id`, `frame_id`, `timestamp_ms` | 结果来源与时间 |
| `model_id`, `model_type`, `model_index` | 模型身份和通道内序号 |
| `box_color` | 框颜色 |
| `keypoints`, `keypoint_scores` | 姿态关键点和分数 |
| `text_result` | 文本类结果 |
| `boxMask` | 分割掩码 |

跟踪在 logic 之前执行，因此 logic 看到的是已更新跟踪字段的结果。`has_target(label)` 和
`target_count(label)` 是整帧快捷查询；必须先确认业务标签与模型 labels 完全一致。

## 参数和配置

`config` 是只读 `ChannelConfig*`。模块专有参数通过以下方法读取：

- `has_param(key)`；
- `param_float(key)`、`param_int(key)`、`param_bool(key)`；
- `param_string(key)`；
- `param_json(key)`，用于 Schema 类型为 `array` 或 `object` 的值。

启动或热重载时，框架按该模块 `logic.json.parameters` 校验并补默认值。参数 key 和类型不应在
C++ 与 manifest 中重复定义成不同契约。

## 类型化 outputs

每帧的 `outputs` 是一份新的 `LogicOutputSet`，logic 可调用：

- `publish_string(key, value)`；
- `publish_number(key, value)`；
- `publish_int(key, value)`；
- `publish_bool(key, value)`；
- `publish_json(key, json)`。

发布的 key/type 必须在 `logic.json.outputs[]` 中以 `string`、`number`、`integer`、`boolean` 或
`json` 声明。logic 未在某帧重新发布的 key 不会自动沿用上一帧；全局逻辑必须把“缺失”和合法的
`0`、`false`、空字符串区分开。

## ROI

`rois` 中的多边形已经缩放到模型输入坐标系。可用接口：

- `roi_count()`、`roi_at(i)`、`roi_polygon_at(i)`、`roi_name_at(i)`；
- `roi_by_name(name)`、`roi_find(ctx, name)`；
- `roi_contains(ctx, box, idx)`；
- `roi_has_target(ctx, label, idx)`、`roi_count_target(ctx, label, idx)`；
- `roi_index_of(box)`。

命中规则使用检测框中心点。`ROI_ALL == -1` 表示所有 ROI 的并集；没有配置 ROI 时表示整帧不
设限。`ROI_NONE == -2` 表示不存在/未命中，不能误当作 `ROI_ALL`。运行快照会排除少于三个点的
区域；若直接调用静态 `point_box_in_poly()`，空/无效 polygon 会返回 1，而 `roi_contains()`、
`roi_has_target()`、`roi_count_target()` 对无效的指定 ROI 返回 0。生产逻辑优先使用组合查询接口，
业务若要求“必须画有效区域”还应先确认区域存在。

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
`display_canvas()` 首次调用会克隆一份可写的模型尺寸 BGR 画布，取帧失败时返回空 Mat，调用方应
检查 `empty()`。两者都只改变当前显示底图，不改变推理帧或事件媒体源；系统标注和 draw 指令仍会
在显示出口叠加。

## 状态和生命周期

`state` 的类型是 `std::shared_ptr<void>*`，每个通道实例独立。典型初始化：

```cpp
if (!*ctx->state)
    *ctx->state = std::make_shared<MyState>();
auto &state = *std::static_pointer_cast<MyState>(*ctx->state);
```

不要缓存 `ctx`、`results`、`rois`、`draw_cmds` 或帧指针。参数热重载是否保留状态取决于变更参数
的 `x-hot-reload` 策略；切换 logic 会建立与新模块匹配的状态。

## 跨通道快照

`get_channel_frame_snapshot(configuredId, &out)` 会在目标通道锁内原子取得帧、results、outputs、
绘制和发布元信息，并在返回前释放锁；带图快照会深拷贝图像，属于高级且更昂贵的接口。
通常的跨通道业务应由通道发布 outputs，再交给全局 logic 聚合。辅助方法还有
`get_channel_logic_name(id)` 和 `channel_has_logic(id, name)`。
