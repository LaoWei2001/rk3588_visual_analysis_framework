# 业务 SDK：按功能找接口

这里是开发自定义逻辑的公共接口入口。头文件提供可调用的函数声明、数据类型、参数说明和
必要的模板/内联实现；普通函数的实现由 `engine/src/` 编译进程序，业务无需包含内部头文件。
通道逻辑从 `<channel.h>` 开始，全局逻辑从 `<global.h>` 开始，其他能力按需包含。

## 功能速查

| 想做什么 | 使用的接口 | 头文件 |
| --- | --- | --- |
| 创建通道逻辑、接收按钮动作 | `REGISTER_LOGIC`、`REGISTER_LOGIC_ACTION`、`LogicAction` | [channel.h](channel.h)、[actions.h](actions.h) |
| 读取当前帧和目标 | `ctx->business_frame()`、`source_frame()`、`results`、`inference_valid` | [channel.h](channel.h)、[types.h](types.h) |
| 查询目标数量、位置、姿态 | `has_target()`、`target_count()`；`AlgoResult::box_center()`、`box_contains()`、`is_pose()` | [channel.h](channel.h)、[types.h](types.h) |
| 组合类别、置信度、ROI 条件，获取数量和最佳目标 | `TargetQuery`、`ctx->query_targets()`、`TargetQueryResult` | [channel.h](channel.h) |
| 查找 ROI、判断目标是否在区域内 | `roi_find()`、`roi_contains()`、`roi_has_target()`、`roi_count_target()`、`ctx->roi_by_name()` | [channel.h](channel.h) |
| 获取统一坐标和时间 | `business_width/height/size/center()`、`timestamp_ms`、`dt_ms`、`time_str()`、`datetime()` | [channel.h](channel.h)、[coordinates.h](coordinates.h) |
| 读取 Web 上配置的业务参数 | `has_param()`、`param_int/float/bool/string/json()` | [channel.h](channel.h)、[global.h](global.h) |
| 保存跨帧或跨 tick 状态 | `ctx->get_state<MyState>()`、`gctx->get_state<MyState>()` | [channel.h](channel.h)、[global.h](global.h) |
| 画框、圆、线、文字和区域 | `draw_rect/circle/line/text/polyline/poly_filled()` | [drawing.h](drawing.h) |
| 自由处理画面像素 | `ctx->display_canvas()`、`replace_display_frame()`、`blend_display_mask()` | [channel.h](channel.h)、[drawing.h](drawing.h) |
| 在自有图像上写中文、测量文字 | `draw_text_unicode()`、`measure_text_unicode()`、`text_overlay_available()` | [drawing.h](drawing.h) |
| 向全局逻辑发布数据 | `ctx->publish_int/number/bool/string/json()` | [channel.h](channel.h)、[outputs.h](outputs.h) |
| 聚合多个通道 | `REGISTER_GLOBAL_LOGIC`、`gctx->inputs()`、`input_at()`、`ChannelInput::read_*()/get_*()` | [global.h](global.h) |
| 获取某通道同一版本的证据 | `get_channel_frame_snapshot()`、`ChannelFrameSnapshot` | [channel.h](channel.h)、[global.h](global.h)、[snapshot.h](snapshot.h) |
| 提交事件、附带业务字段 | `EventRequest`、`event_field()`、`event_json_field()`、`report_event()` | [events.h](events.h) |
| 控制/读取 GPIO、查询引脚 | `gpio_set_output()`、`gpio_set_input()`、`gpio_read_input()`、`gpio_get_pin_info()` | [gpio.h](gpio.h) |
| 开关某通道推理 | `logic_control_set_channel_inference()`、`logic_control_get_channel_inference()` | [control.h](control.h) |
| 查看通用配置和模型信息 | `ctx->config`、`gctx->config`；均为只读借用数据 | [config_types.h](config_types.h) |
| 解析或生成 JSON | `cJSON_Parse()`、`cJSON_CreateObject()`、`cJSON_Delete()` 等 | [json.h](json.h) |
| 查询 SDK 版本 | `VISION_SDK_VERSION_*` | [version.h](version.h) |

## 简单调用

```cpp
#include <channel.h>
#include <drawing.h>

struct MyState
{
    int calls = 0;
};

static void logic_example(ChannelContext *ctx)
{
    if (!ctx) return;
    MyState *state = ctx->get_state<MyState>();
    if (!state) return;
    ++state->calls;

    const int count = ctx->target_count("person");
    const std::string text = "count=" + std::to_string(count);
    draw_text(ctx, text.c_str(), {20, 30});
}

REGISTER_LOGIC(logic_example);
```

`person` 是示例标签，使用时换成模型实际标签。状态在首次调用时构造，同一个模块的帧回调
和 Action 使用同一种状态类型；不同实例分别保存。同一实例后续调用不会再次构造或重置。
需要初始化参数时可写 `ctx->get_state<MyState>(args...)`，参数只用于首次构造。

函数返回的帧、状态、配置和输入指针默认只在当前回调期间借用；需要跨回调保存图像时显式
复制。计时间隔用 `timestamp_ms`，显示日期用 `time_str()`。绘图默认使用统一业务坐标。
发布变量、业务参数和事件字段仍需在所属模块的 `logic.json` 声明。

## 组合筛选目标

```cpp
TargetQuery query;
query.labels = {"person", "car"};
query.min_score = 0.7f;
query.roi = roi_find(ctx, "entrance");
query.anchor = TargetAnchor::BottomCenter;

const auto selected = ctx->query_targets(query);
if (!selected.valid())
{
    // selected.status 可区分区域错误、查询条件错误和推理不可用。
    // target_query_status_name(selected.status) 提供对应的状态字符串。
    return;
}
ctx->publish_int("count", selected.count);
if (selected.best)
    draw_rect(ctx, selected.best->box);
```

此片段在已判空的通道回调中使用，发布的 `count` 需在模块清单声明。
不指定条件时 `ctx->query_targets()` 查询整帧全部类别；多个标签之间取任意命中，
标签、置信度与区域条件之间必须同时满足。完整状态和边界约定见
[目标查询说明](../../docs/sdk/target-query.md)。

## 哪些能力放进 SDK

- 放通用能力：帧访问、目标/ROI 查询、绘图、时间、状态、参数、通道通信、事件、GPIO。
- 新接口必须有明确输入、输出、失败含义和生命周期；操作对象由参数或上下文指定。
- 具体标签、阈值、工艺流程、状态机和报警策略由项目模块配置或实现。
- 吊钩偏斜、安全帽判断、行车运动、项目标定和继电器联动规则保留在各自 `projects/` 模块。
- 线程启动/停止、注册表查询、渲染缓存、媒体所有权和 GPIO 全局初始化/释放由引擎管理。
- 业务代码只包含这里的 SDK 头文件、标准库/OpenCV 和本项目的业务辅助头文件。

多个项目都需要某段代码时，先去掉项目标签、阈值和流程，再判断是否已成为通用操作；
不能仅因为两个行车模块共用，就把行车辅助函数搬进 SDK。

完整边界、构建方式和参考设计见 [SDK 说明](../../docs/sdk/README.md)。
