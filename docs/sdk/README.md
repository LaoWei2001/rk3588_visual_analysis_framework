# 公共 SDK

业务代码的正式接口位于 `engine/include/`。当前 SDK 版本为 `0.1.0`，提供
C++ 源码接口，依赖 C++11 或更高版本及 OpenCV。GPIO 和 JSON 头文件也可由 C 使用。
本阶段没有新增系统依赖，也没有改变推理、显示、采集或线程调度流程。

按功能查找函数，直接打开 [engine/include/README.md](../../engine/include/README.md)。
该目录是业务 SDK 的接口仓库；普通函数的实现仍由 `engine/src/` 中的 `.cpp` 提供。

## 接口设计原则

采用能力分组、统一调用形式、明确数据所有权的方式组织接口。参考
[raylib 的功能速查](https://www.raylib.com/cheatsheet/cheatsheet.html)与
[STM32 HAL 的公开/私有接口和命名约定](https://dev.st.com/stm32cube-docs/stm32g5xx-hal-drivers/2.0.0/en/docs/overview/hal_ll_drivers_files.html)，
公共层提供通用操作，引擎负责资源和运行机制，项目组合这些操作实现规则。

| 位置 | 职责 | 示例 |
| --- | --- | --- |
| `engine/include/` | 可复用业务 API 的声明、类型、模板与说明 | 目标查询、ROI、绘图、参数、状态、事件、GPIO |
| `engine/src/` | 公共 API 的实现和引擎私有接口 | 解码、推理调度、注册分发、渲染缓存、媒体持久化、线程生命周期 |
| `projects/` | 项目算法、阈值和业务流程 | 吊钩偏斜、安全帽、行车运动、标定、告警联动 |

公共头文件不得依赖 `engine/src/`、项目模块或 RKNN/GStreamer/RGA 等后端接口。
新 API 应以参数表达对象和条件，不硬编码业务类别、现场阈值或报警策略。
`config_types.h` 提供通用配置的只读查看；新增模块参数放所属模块的 Schema，不扩展中央配置。

## 开发入口

通道逻辑包含 `<channel.h>`，跨通道逻辑包含 `<global.h>`。
绘图和事件分别使用 `<drawing.h>`、`<events.h>`：

```cpp
#include <channel.h>

static void logic_person_count(ChannelContext *ctx)
{
    if (!ctx) return;
    ctx->publish_int("person_count", roi_count_target(ctx, "person", ROI_ALL));
}

REGISTER_LOGIC(logic_person_count);
```

在该模块的 `logic.json` 中声明输出 `person_count`，类型为 `integer`。模块源码和清单
继续放在 `projects/modules/` 或 `projects/global_modules/`，无需修改引擎或通用 Web。
完整清单格式与开发流程见 [业务工作区说明](../../projects/README.md)。

完整的参数、ROI、跨帧计时、绘图、输出和事件组合示例见
[区域持续占用模块](../../projects/modules/logic_roi_dwell_demo/README.md)。
组合类别、置信度与区域条件，使用 `TargetQuery` 和 `ctx->query_targets()`，见
[通用目标查询](target-query.md)。一次获取数量和最佳目标，并明确区分有效空结果与查询失败。

| 头文件 | 用途 |
| --- | --- |
| `channel.h` | `ChannelContext`、ROI 查询、通道注册与动作注册 |
| `global.h` | `GlobalContext`、有效通道输入、全局注册与动作注册 |
| `types.h`、`coordinates.h` | 检测结果、ROI、绘图指令、时间与固定业务坐标 |
| `drawing.h` | 绘图指令、显示画布辅助和 UTF-8 文字接口 |
| `events.h` | `EventRequest`、字段和事件提交结果 |
| `config_types.h` | 回调中借用的只读通道/全局配置数据，不包含加载和热更新实现 |
| `snapshot.h`、`outputs.h` | 跨通道数据和同帧媒体快照；媒体所有权对业务不可访问 |
| `actions.h` | 动作请求与返回结果 |
| `control.h` | 线程安全的通道运行时推理开关 |
| `gpio.h` | GPIO 操作和诊断，按需包含 |
| `json.h` | 可选 cJSON 辅助，复用现有框架实现与许可证 |
| `version.h` | SDK 版本宏和通道上限 |

按功能直接包含声明和定义所在的头文件；GPIO、JSON、事件和全局逻辑需要时显式包含。
OpenCV 图像处理函数和标准库函数也应包含各自的头文件。

## 业务与内部实现的边界

业务编译目标 `vision_business` 只使用 `Vision::SDK` 导出的公共头文件路径和 OpenCV 路径，
没有 `engine/src/`、RKNN、GStreamer 或 GTK 的项目包含路径。引擎自己的目标保留这些
内部依赖。业务中的局部辅助头文件仍可正常使用。

`engine/src/` 保存实现和私有接口，业务不得直接包含该目录的文件。`APP_CTRL`、通道锁、
NPU 队列和运行快照实现不由 SDK 导出。上下文中的取帧、绘图、输出及全局输入绑定，
以及快照中的媒体引用，均为私有字段，由内部 `context_access.h` 绑定。

注册表查找/枚举位于 `src/logic/core/logic_registry.h`；内部 `RenderParams` 和文字缓存入口
位于 `src/display/display.h`；GPIO 全局初始化、释放和启动恢复位于 `src/gpio/gpio_runtime.h`。
业务使用 `REGISTER_*`、`draw_*` 和 GPIO 控制/查询接口，不负责这些全局生命周期。

接口数据的唯一完整定义在公开头文件中，内部实现也直接包含这些定义，不保留转发头。已有 `ChannelContext`、
`AlgoResult`、绘图函数与注册宏继续使用原名称，不要求业务改名。

## 生命周期和性能约定

- 回调中的上下文、配置、参数、输入和取帧返回指针仅在当前回调期间有效。
- `business_frame()` / `model_frame()` 提供固定 640×640 业务图；`source_frame()`
  提供源分辨率原图。仅在调用时转换，同一帧复用缓存。需要异步持有或修改图片时显式复制。
- 跨帧状态使用 `ctx->get_state<T>()` / `gctx->get_state<T>()`，首次构造、后续复用，
  每个实例独立。同一模块的普通回调与 Action 保持同一种状态类型；借用指针不跨回调保存。
  原始 `state` 存储仍可用于已有模块，不要用函数内静态变量存放每通道状态。
- 使用 `timestamp_ms` / `dt_ms` 计时。全局回调可能合并多次通道更新，输出不是无损事件队列。
- 回调应及时返回，避免阻塞网络或磁盘操作。事件投递与媒体生成继续由框架后台处理。
- 此次调整不增加逐帧分配、图像转换、复制、锁或线程。内部绑定辅助函数是简单内联赋值，
  全局证据快照的原有实现移至 `.cpp`，取证语义不变。

## 业务源码升级

业务源码直接使用 `engine/include/` 下的公开头文件；旧路径和只做包含转发的文件已移除。
通道使用 `channel.h`，全局使用 `global.h`，GPIO 使用 `gpio.h`，
JSON 使用 `json.h`。业务尺寸使用 `ctx->business_width/height()`。

直接访问内部控制块或上下文内部绑定字段的代码需要迁移到公开方法，这类使用不属于
公共接口兼容范围。本阶段保持已有正常业务接口；当前仓库业务模块已经迁移到新包含路径。

版本宏用于源码兼容识别，不承诺独立编译插件的二进制 ABI。SDK 更新后重新构建应用，
不能单独替换旧应用中的结构或业务目标文件。静态 SDK 库、独立安装包和动态业务插件
属于后续工作。

## 验证

```bash
cmake -S engine -B build/engine -DBUILD_SDK_TESTS=ON
cmake --build build/engine --parallel 4
(cd build/engine && ctest --output-on-failure -R '^sdk_')
python3 tools/build/generate_logics_catalog.py --check
```

SDK 测试独立编译每个公开头文件和业务样例，检查全部业务源码的实际包含依赖（包括相对路径），
验证引擎私有函数/类型不可见、内部绑定不可访问，并执行跨帧状态初始化、复用、实例隔离和释放测试。
其他已有测试继续验证坐标映射、媒体快照、采集、跟踪和显示行为。

仓库自带的 `logic_roi_dwell_demo` 还通过 `sdk_business_workflow_regression` 执行业务验收：
使用真实 SDK 实现验证计时边界、目标/ROI 查询、参数解析、状态隔离、变量发布与最终像素绘制。
事件出口由测试接收器替代，以便准确检查请求字段、提交顺序及失败重试；不代表已经完成远端投递。
具体场景、录像结果和覆盖范围见 [实战验收记录](acceptance.md)。

源码变更后需重新构建和部署应用。已经生成的应用发布包、离线包目录和压缩包不会随源码
自动更新，按原有打包流程重新生成。
