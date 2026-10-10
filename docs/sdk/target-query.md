# 通用目标查询

公开类型和方法在 [channel.h](../../engine/include/channel.h)，实现位于引擎内部的
`channel_logic.cpp`。业务只包含 `<channel.h>`，无需新增包含路径。

`ctx->query_targets(query)` 一次遍历当前帧结果，返回匹配数量、最高置信度目标和查询状态。
不构造匹配目标数组，不复制目标中的图像或姿态数据，不触发取帧转换。
`TargetQuery` 的标签列表由调用方维护，需要时可以保存在跨帧状态中复用。

## 用法

在通道回调中、确认 `ctx` 非空后：

```cpp
TargetQuery query;
query.labels = {"person", "car"};
query.min_score = 0.7f;
query.roi = roi_find(ctx, "entrance");
query.anchor = TargetAnchor::BottomCenter;

const auto selected = ctx->query_targets(query);
if (!selected.valid())
    return;

ctx->publish_int("target_count", selected.count);
if (selected.best)
    draw_rect(ctx, selected.best->box);
```

`target_count` 在所属模块的 `logic.json` 声明为 `integer`。标签替换为模型的实际输出名称。
仅查整帧全部目标可直接写 `ctx->query_targets()`。

## 条件约定

| 条件 | 默认值 | 含义 |
| --- | --- | --- |
| `labels` | 空列表 | 不限类别；非空时匹配其中任意一个标签，区分大小写，不修剪空格 |
| `min_score` | `0.0f` | 必须为有限的 [0,1] 值，匹配置信度大于或等于阈值 |
| `roi` | `ROI_FRAME` | 查整帧，忽略通道已经配置的区域 |
| `anchor` | `TargetAnchor::Center` | 用框中心判断区域归属 |

各项条件必须同时满足。`roi` 的其他取值：

- `ROI_ALL`：全部已配置区域的并集；没有区域时查整帧，与既有接口的语义一致。
  所有已配置区域都须至少三个顶点，任何一个不足三个顶点时返回 `INVALID_ROI`。
- 非负编号：仅查对应区域，通常来自 `roi_find(ctx, "name")`。
- `ROI_NONE`：名字不存在，返回 `ROI_NOT_FOUND`；不会回退为整帧。
- 其他负数：返回 `INVALID_QUERY`。

`TargetAnchor::Center` 使用 `(x + width/2, y + height/2)`，整数除法取半宽、半高，
与旧 ROI 查询一致。`BottomCenter` 使用 `(x + width/2, y + height)`。
坐标均为固定 640×640 业务坐标，包含多边形边界。区域顶点数量检查不等同于完整的多边形合法性检查。

置信度非有限、超出 [0,1] 或框宽/高非正的检测结果会被跳过。
同一目标同时落在多个区域只计一次；重复标签不重复计数。
不同模型输出的独立检测记录不去重；第一版不提供模型筛选、跟踪优先或距离排序。

## 结果及生命周期

| `selected.status` | 含义 |
| --- | --- |
| `OK` | 查询有效；没有目标时 `count=0`、`best=nullptr` |
| `INFERENCE_UNAVAILABLE` | 推理关闭、当前批次无效或没有绑定结果容器 |
| `ROI_NOT_FOUND` | 区域名称不存在或编号越界 |
| `INVALID_ROI` | 所选区域不足三个顶点 |
| `INVALID_QUERY` | 阈值、区域选择值或判断点枚举不合法 |

`selected.valid()` 仅在 `status == OK` 时为真。所有失败结果均为 `count=0`、`best=nullptr`。
调用 `target_query_status_name(selected.status)` 可获取状态字符串。
优先校验查询条件，再校验区域，最后检查推理可用性，便于发现配置错误。

`best` 为最高置信度匹配目标的只读借用指针，同分取原检测结果顺序中的第一个。
只在当前回调内使用；不要缓存到跨帧状态、交给异步线程，或先修改/重分配 `ctx->results` 再使用。
需要跨帧记录目标位置时，复制框、标签、分数或跟踪 ID 等所需数据。

多模型通道的 `inference_valid` 可能表示部分模型成功，因此 `OK` 只说明当前结果批次可用，
不能据此断定每个子模型都成功，也不能把某类别的零数量解释为其所属模型必定正常运行。

## 兼容与验证

原有 `has_target()`、`target_count()`、`roi_has_target()` 和 `roi_count_target()` 保留：
它们仍按旧约定查询，不自动判断推理有效性。新增的 `ROI_FRAME` 也可用于 ROI 自由函数，
明确选择整帧；旧 `ROI_ALL` 和非法编号的行为不变。

独立业务测试 [test_target_query.cpp](../../engine/tests/sdk/test_target_query.cpp) 仅以公开 SDK
路径编译，执行真实引擎查询实现，覆盖条件组合、阈值边界、判断点、状态区分、坏数据与旧接口兼容。
现有 [区域报警示例](../../projects/modules/logic_roi_dwell_demo/README.md) 已使用新查询。

```bash
cmake -S engine -B build/engine -DBUILD_SDK_TESTS=ON
cmake --build build/engine --parallel 4
(cd build/engine && ctest --output-on-failure -R '^sdk_')
```
