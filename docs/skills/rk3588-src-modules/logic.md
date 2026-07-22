# `src/logic`：通道与全局业务逻辑

## 文件边界

- `core/channel_logic.h/.cpp`：`ChannelContext`、ROI 查询、绘制命令、通道 logic/action 注册表。
- `core/logic_parameters.h/.cpp`：模块参数 Schema 解析、默认值、类型化参数表和热重载差异。
- `core/global_logic.h/.cpp`：跨通道轮询上下文、实例线程和注册表。
- `core/logic_common.h`：业务 logic 的聚合 include。
- `modules/logic_xxx/logic.cpp`：该业务逻辑实现；复杂模块可继续拆 `.cpp/.cc/.cxx/.h/.hpp`。
- `modules/logic_xxx/logic.json`：除 logic ID 外，该逻辑的参数 Schema、动作、上报字段和 Web 元数据唯一真源。
- `catalog.json`：全局逻辑和模型类型等共享能力，不放具体 channel logic。
- App 根目录 `logics.json`：构建脚本生成的 Web 清单，不是手工维护源文件。

## 通道 logic 执行语义

`channels[].logic` 是可选后处理步骤。配置了模块时，推理通道在新结果到达时执行，非推理通道在帧入口以空 results 逐帧执行；未配置时框架直接跳过业务调用，但仍保存当前帧和模型结果供通用画面绘制、快照及跨通道读取。动作在本帧 logic 前处理。每次调用使用栈上的 `ChannelContext`，跨帧数据存入 `*ctx->state`；框架在切换 logic 时清空旧状态、结果、绘制和 canvas。

最小实现：

```cpp
#include "logic/core/logic_common.h"

static void logic_person(ChannelContext *ctx)
{
    if (ctx->has_target("person"))
        draw_text(ctx, "person", {20, 40}, {0, 0, 255}, 0.8, 2,
                  DrawCommand::ALL);
}

REGISTER_LOGIC(logic_person);
```

`REGISTER_LOGIC` 通过文件作用域静态对象在 `main()` 前注册，并把唯一的 C++ 函数标识符字符串化为 config/Web/外部 API 使用的 logic ID。源 `logic.json` 不声明 `name`；构建生成器把函数名注入生成的 Web `logics.json`。CMake 自动收集模块 C++ 源文件。

## `ChannelContext` 关键数据

- `frame`：与当前结果匹配的 BGR 模型输入帧；`src_width/src_height` 才是源分辨率。
- `results`：可修改的本帧结果，含模型来源、tracker、pose/seg 等扩展字段。
- `config`：当前通道只读配置；不要缓存其指针跨调用。
- `logic_parameters`：当前模块类型化参数表；业务通过 `param_float/int/bool/string/json()` 读取。
- `rois`/`roi`：模型坐标系的多 ROI 和首 ROI 兼容指针。
- `draw_cmds`：本帧输出；调用 `draw_*` 追加命令。
- `state`：框架持有的 `shared_ptr<void>` 槽，logic 自行转换为具体状态类型。
- `timestamp_ms`：单调时间；`unix_ms`、`time_hms()`、`time_str()`、`datetime()`：墙钟时间。
- `infer_enabled`：本次是否实际启用推理。

`display_canvas()` 首次调用会克隆 `frame` 并只替换显示底图，不改变推理帧或告警原图。需要跨通道同帧数据时使用 `get_channel_snapshot()`。

## ROI 与绘制

`roi_find()` 返回索引或 `ROI_NONE`；`ROI_ALL` 表示所有 ROI 并集，无 ROI 时按整帧处理。`roi_contains` 用框中心判断；`roi_has_target`/`roi_count_target` 按类别查询。不要把 `ROI_NONE=-2` 当成 `ROI_ALL=-1`。

`ctx->roi_count()/roi_at()/roi_index_of()` 是成员函数，调用时隐式使用 `this=ctx`；`roi_contains(ctx,...)/roi_has_target(ctx,...)/roi_count_target(ctx,...)` 是显式接收上下文的自由函数，统一封装 `ROI_ALL/ROI_NONE/具体索引` 规则并能在内部处理空 `ctx`。这主要是 API 组织和兼容选择，不代表自由函数更快；完整比较见 `../rk3588-channel-logic/references/channelcontext-api.md`。

绘制 API 包括矩形、圆、线、文字、折线和填充多边形。坐标、半径、线宽均基于模型输入坐标系，由 player 映射到目标画面。Target 位掩码为：`DISPLAY=1`、`IMAGE=2`、`VIDEO=4`、`UPLOAD=6`、`ALL=7`。当前告警叠加图片使用 `DISPLAY|IMAGE`，叠加视频使用 `DISPLAY|VIDEO`，所以 DISPLAY 层会被媒体复用；原始媒体模式不绘制。

`ctx->draw_cmds` 是每次 logic 调用的命令输出队列；`draw_*` 内部构造 `DrawCommand` 并 `push_back`，业务 logic 通常不直接访问。logic 返回后队列被移交给实时显示，告警图片和录像按各自 Target mask 复制并延迟渲染。它不是跨帧状态，不能缓存指针。完整上报对照示例见 `../rk3588-channel-logic/references/examples/logic_upload_teach.md`。

## 通道动作

实现 `ChannelActionResult handler(ChannelContext*, const ChannelAction&)` 并用 `REGISTER_LOGIC_ACTION` 注册。动作名和按钮元数据位于模块 `logic.json`；处理器负责验证 `payload_json` 和返回 `handled/message`。详细链路见 [control.md](control.md)。

## 告警

使用 `report_alarm()`，字段元数据同步写入模块 `logic.json.report_fields`。同一次调用可以由 Web 配成服务器图片、Dify 图片和 Dify 视频多条 delivery；业务 logic 不选择投递地址，也不直接执行 HTTP/Dify。见 [alarm.md](alarm.md) 和统一上报教学示例 `../rk3588-channel-logic/references/examples/logic_upload_teach.md`。

## 模块参数

普通业务参数在模块 `logic.json.parameters.properties` 声明，通过 `ctx->param_*()` 读取。Schema 同时驱动 C++ 默认值/校验、Web 表单和 `preserve_state`/`reset_state`/`restart_required` 热重载策略。参数值保存在通道 `logic_parameters` 对象；逐帧读取不可变类型化快照，不解析 JSON。详细指南见 `../rk3588-channel-logic/references/adding-config-parameter.md`。

## 全局 logic

每个启用的 `GlobalLogicConfig` 有独立线程、轮询间隔和 `state`。`GlobalContext` 提供受监控通道列表、tick、是否有新推理、最新推理通道以及安全快照。只看本通道且要求每帧响应时用 channel logic；跨通道汇总或周期巡检用 global logic。

当前全局 logic 不是通道 logic 的自注册宏模式：实现函数后需要在 `global_logic.cpp::global_logic_register()` 显式调用 `register_global_logic()`。配置位置是 `global.global_logics`，热更新会停止并重建全部实例。

## 二次开发硬约束

- logic 运行在实时处理路径，不做阻塞网络、长时间磁盘 IO 或无界循环。
- 不在持有框架锁时执行复杂业务；通过 context/快照复制数据。
- 所有可持久状态放入 `ctx->state`，不要用跨通道无锁全局变量。
- 修改公共状态结构时同步检查热切 logic、断流恢复、动作处理和 alarm 快照语义。
