# 双通道变量聚合与上报示例

这套示例对应两个可直接选择的模块：

- 通道逻辑 `logic_global_input_demo`：每帧发布 `target_count`、`local_alarm`、`risk_ratio`；
- 全局逻辑 `global_two_channel_demo`：只接受两路画布输入，读取两路变量和参数，组合判断后统一上报。

同一个通道逻辑模块可以被多个视频通道复用。每个通道仍有独立的参数和运行上下文，不需要复制出
“入口版 logic”和“出口版 logic”两套代码。

## 画布接线

```text
视频流 A → 模型 A → logic_global_input_demo ─┐
                                               ├→ global_two_channel_demo → 上报配置
视频流 B → 模型 B → logic_global_input_demo ─┘
```

1. 放置两个视频流节点，分别配置视频地址和通道 ID。
2. 每路视频后连接自己的模型节点。
3. 每个模型后连接一个“逻辑函数”节点，两个逻辑节点都选择
   `示例：向全局逻辑发布通道指标`。
4. 通道 A 可配置 `channel_role=入口`、`target_label=person`、`local_count_threshold=1`；
   通道 B 可配置 `channel_role=出口`、`target_label=person`、`local_count_threshold=2`。
5. 放置一个“全局逻辑”节点，选择 `示例：双通道变量聚合与上报`。
6. 从两个通道逻辑节点底部的全局输出口，分别连接到这个全局逻辑节点左侧输入口。
7. 从全局逻辑节点右侧连接一个已有的“上报配置”节点，选择事件类型
   `双通道组合告警`，再配置图片、视频、纯数据和远端投递目标。

全局节点配置建议先使用：

| 参数 | 示例值 | 作用 |
|---|---:|---|
| 两路目标总数阈值 | 3 | A、B 两路人数之和达到 3 才满足总数条件 |
| 要求两路都达到局部阈值 | 关闭 | 关闭时只判断总数；开启后还要求两路 `local_alarm=true` |

## 数据如何流动

通道逻辑用以下代码公开运行变量：

```cpp
ctx->publish_int("target_count", target_count);
ctx->publish_bool("local_alarm", local_alarm);
ctx->publish_number("risk_ratio", risk_ratio);
```

全局逻辑从本 tick 固定的轻量快照读取每个通道最近一次公开的变量，以及产生这些变量时使用的同代参数：

```cpp
const ChannelLogicSnapshot *channel = gctx->channel(channel_id);
if (!channel || !channel->has_publication || channel->publication_age_ms > max_age_ms)
    return;

int64_t count = 0;
bool local_alarm = false;
if (!channel->outputs.try_get_int("target_count", &count) ||
    !channel->outputs.try_get_bool("local_alarm", &local_alarm))
    return;

std::string role = channel->parameters.get_string("channel_role");
int64_t local_threshold = channel->parameters.get_int("local_count_threshold");
```

满足组合条件时仍使用与通道逻辑相同的 `EventRequest`：

```cpp
EventRequest request;
request.event_type = "two_channel_combined_alarm";
request.source_channel_id = source_channel_id;
request.fields = {event_field("total_count", total_count)};
report_event(gctx, request);
```

示例会动态选择 `risk_ratio` 更高的通道作为事件来源和视频来源。上报图片始终包含两路输入，
并按全局显示窗格拼接；是否叠加标注仍由上报节点的图片叠加选项决定。上报节点里的“事件视频
来源通道”是没有显式设置 `request.source_channel_id` 时的默认值。事件字段会携带两路通道的角色、
计数和局部阈值；事件来源信息会记录最终选择的通道 ID。

## 运行约束

- 全局节点必须正好连接两个通道逻辑；多接或少接时直接跳过分析和上报。
- 示例中两路都选择 `logic_global_input_demo`；实际也可以连接其他模块，只要它们声明并发布
  `target_count`、`local_alarm`、`risk_ratio` 三个同类型输出，并提供 `channel_role`、
  `local_count_threshold` 两个同类型参数。这使全局逻辑依赖数据契约而不是模块名。
- `target_label` 必须与各自模型的标签一致；如果模型标签不是 `person`，应在两个通道节点分别修改。
- 条件持续成立期间只上报一次；条件解除后，下一次成立才再次上报。
- 图片和视频媒体仍完全服从画布上报节点的配置，全局逻辑不直接编码文件或访问远端接口。
- 教学示例和生产规则默认都使用本 tick 固定的 `ChannelLogicSnapshot`；变量、同代参数、版本和新鲜度
  已在一个对象里，不复制图像。确实需要原始结果或画面时再调用 `get_channel_frame_snapshot()`。
- 不同通道不保证来自同一采集时刻；跨通道时序规则应比较各自 `frame_steady_ms`，并定义允许偏差。
- `missed_revisions > 0` 表示轮询期间跳过了中间状态；不能丢的瞬时业务必须设计事件队列。

模块源码位置：

- `vision_analysis/src/logic/modules/logic_global_input_demo/`
- `vision_analysis/src/logic/global_modules/global_two_channel_demo/`
