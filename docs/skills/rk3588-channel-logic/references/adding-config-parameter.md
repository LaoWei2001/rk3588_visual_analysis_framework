# 通道逻辑专有参数：Schema、C++ 调用、Web 配置与热重载

本文面向在 `vision_analysis` 上新增或维护 `logic_xxx` 的二次开发者，说明当前已经落地的模块参数体系。本文以现行源码为准，替代旧的“修改 `ChannelConfig`、增加 `REG_C`、手写 `logics.json`”流程。

## 先记住结论

给某个通道逻辑增加一个普通业务参数时，开发代码通常只修改该逻辑目录中的两个文件：

```text
vision_analysis/src/logic/modules/logic_xxx/
├── logic.cpp       # ctx->param_*() 读取参数
└── logic.json      # 参数 Schema、Web 文案和热重载策略
```

不再为每个逻辑参数修改以下中央文件：

- `src/config/config.h`；
- `src/config/config_init.cpp`；
- `src/core/app_ctrl.cpp`；
- Web 前端字段列表；
- 生成后的应用根目录 `logics.json`。

具体通道的参数值仍保存在运行配置中，但统一放入该通道的 `logic_parameters` 对象：

```json
{
  "id": 0,
  "logic": "logic_person_dwell",
  "logic_parameters": {
    "dwell_sec": 8,
    "overlay_enabled": true
  }
}
```

“增加参数定义/修改 C++ 代码”需要重新编译和部署；部署完成后，“修改参数值”才是 Web 热重载，不需要再次编译。

## 参数应该放在哪里

不要把所有变量都放进 `logic_parameters`。先判断它属于哪一层：

| 数据性质 | 正确位置 | 读取方式 |
|---|---|---|
| 某个 logic 独有的只读业务参数，例如停留秒数、报警阈值、显示开关 | 模块 `logic.json → parameters` | `ctx->param_*()` |
| 视频流和通道天然具有的信息，例如源分辨率、FPS、URL、USB 规格 | `ChannelContext` / `ChannelConfig` 公共字段 | `ctx->src_width`、`ctx->config->stream` 等 |
| 逻辑运行中会变化的计数、计时、闩锁、去重集合 | 每通道 `ctx->state` | logic 自己读写 |
| 显示、模型、stream、ROI、上报策略等系统配置 | 现有公共配置和专用更新链路 | 使用现有 `ctx`/配置 API |

`logic_parameters` 是“模块专有、用户可配置、运行时只读”的参数表，不是逻辑状态容器，也不是系统公共配置的替代品。

## 整条数据链

```text
模块 logic.json + REGISTER_LOGIC(logic_func)
  ├─ 构建期校验：函数注册、参数键、访问器类型、默认值和范围
  ├─ 生成最小 C++ Schema → 嵌入可执行文件
  │    └─ 启动/热重载时校验配置并生成类型化不可变参数表
  └─ 正常打包时生成 App 根目录 logics.json
       └─ Web 后端读取 → 前端自动渲染控件

Web 保存当前运行配置
  └─ channels[].logic_parameters
       └─ C++ 配置监控线程检测文件变化
            ├─ 校验失败：拒绝新配置，继续使用旧运行快照
            └─ 校验成功：按 x-hot-reload 策略原子发布新快照
                 └─ 下一次 logic 调用通过 ctx->param_*() 取得新值
```

JSON 只在启动或热重载时解析，不在逐帧 logic 中重复解析。

## 完整示例：新增人员停留逻辑

### 1. 创建模块目录

```text
vision_analysis/src/logic/modules/logic_person_dwell/
├── logic.cpp
└── logic.json
```

一个模块对应一种逻辑类型，不对应一个实际视频通道。多个通道可以选择同一模块，每个通道拥有独立的参数值和 `ctx->state`。

### 2. 在 logic.json 声明参数

```json
{
  "label": "人员停留报警",
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "dwell_sec": {
        "type": "number",
        "title": "滞留时间",
        "description": "人员连续停留达到该时间后报警。",
        "minimum": 0.5,
        "maximum": 300,
        "default": 5,
        "x-step": 0.5,
        "x-unit": "秒",
        "x-hot-reload": "reset_state"
      },
      "overlay_enabled": {
        "type": "boolean",
        "title": "显示状态文字",
        "description": "是否在实时画面显示累计停留时间。",
        "default": true,
        "x-hot-reload": "preserve_state"
      }
    }
  },
  "report_fields": []
}
```

硬性规则：

- `parameters.type` 必须是 `"object"`；
- `additionalProperties` 必须是 `false`，防止拼错参数名后静默运行；
- 每个参数必须有与类型匹配的 `default`；
- 源 `logic.json` 不写 `name`；`REGISTER_LOGIC(logic_person_dwell)` 会把函数名自动生成为外部 logic ID；
- 一个模块必须且只能注册一次自己的通道 logic。

### 3. 在同目录 C++ 中读取

```cpp
#include "logic/core/logic_common.h"

#include <memory>

struct PersonDwellState
{
    float elapsed_sec = 0.0f;
    bool alarmed = false;
};

static void logic_person_dwell(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<PersonDwellState>();
    auto &state = *std::static_pointer_cast<PersonDwellState>(*ctx->state);

    const float dwell_sec = ctx->param_float("dwell_sec");
    const bool overlay_enabled = ctx->param_bool("overlay_enabled");

    const bool person_present = ctx->has_target("person") != 0;
    if (person_present)
        state.elapsed_sec += ctx->dt_ms / 1000.0f;
    else
    {
        state.elapsed_sec = 0.0f;
        state.alarmed = false;
    }

    if (overlay_enabled)
    {
        draw_text(ctx,
                  ("dwell=" + std::to_string(state.elapsed_sec)).c_str(),
                  cv::Point(20, 30), cv::Scalar(0, 255, 255), 0.7, 2);
    }

    if (!state.alarmed && state.elapsed_sec >= dwell_sec)
    {
        state.alarmed = true;
        // 在这里调用 report_alarm() 或执行该逻辑需要的快速状态变更。
    }
}

REGISTER_LOGIC(logic_person_dwell);
```

参数访问器与 Schema 类型的推荐对应关系：

| Schema 类型 | C++ 访问器 | 返回值 |
|---|---|---|
| `number` | `ctx->param_float("key")` | `float` |
| `integer` | `ctx->param_int("key")` | `int64_t` |
| `boolean` | `ctx->param_bool("key")` | `bool` |
| `string` | `ctx->param_string("key")` | `std::string` |
| 字符串 `enum` | `ctx->param_string("key")` | `std::string` |
| `array` / `object` | `ctx->param_json("key")` | 紧凑 JSON 字符串 |

`ctx->has_param("key")` 可以检查键是否存在。由于每个 Schema 属性都有默认值，正常解析后声明过的键都会存在。

构建生成器会扫描模块 `.cpp/.cc/.cxx/.h/.hpp` 中使用字符串字面量的 `param_*()` 调用。例如把 `dwell_sec` 错写成 `dwlel_sec`，或者用 `param_bool()` 读取 `number`，构建会直接失败。通过变量动态拼出的键无法做这项静态校验，因此业务代码应优先使用字符串字面量。

### 4. 在通道配置中选择逻辑

```json
{
  "id": 0,
  "enable": true,
  "logic": "logic_person_dwell",
  "logic_parameters": {
    "dwell_sec": 8,
    "overlay_enabled": true
  }
}
```

也可以省略具体参数：

```json
{
  "logic": "logic_person_dwell",
  "logic_parameters": {}
}
```

C++ 会按 Schema 补成 `dwell_sec=5`、`overlay_enabled=true`。配置源文件不一定被反向重写，但运行快照中的值一定已经补全并通过类型校验。“未填写默认值”和“显式填写同一个默认值”在热重载比较中视为没有变化。

## Schema 支持的字段

### 基础类型和校验

| 字段 | 说明 |
|---|---|
| `type` | `string`、`number`、`integer`、`boolean`、`array`、`object` |
| `default` | 必填；同时是 C++ 和 Web 的唯一默认值来源 |
| `minimum` / `maximum` | `number`、`integer` 的运行时范围校验 |
| `enum` | 当前支持字符串枚举；默认值必须在枚举中 |
| `x-hot-reload` | `preserve_state`、`reset_state`、`restart_required` |

整数被限制在 JSON/JavaScript 能精确表达的安全整数范围内。数值必须有限，`NaN` 和无穷大不合法。

对于 `array`/`object`，当前框架校验外层容器类型并提供 JSON 文本；内部元素结构仍应由模块业务代码用 cJSON 等方式解析和校验。不要假设已经实现完整的递归 JSON Schema 校验。

### Web 展示扩展

| 字段 | 作用 |
|---|---|
| `title` | 控件中文名称；缺省显示参数 key |
| `description` | 控件帮助文本 |
| `x-placeholder` | 输入框占位提示 |
| `x-step` | 数字输入步长，必须为正数 |
| `x-unit` | 单位，例如“秒”“像素” |
| `x-widget: "textarea"` | 将字符串显示为多行文本框 |
| `x-ui-hidden: true` | 参数仍可在配置/C++ 中使用，但不在通用 Web 表单显示 |

Web 会预先拦截数字越界、`integer` 输入小数以及 `array/object` 容器类型错误；C++ 仍是最终权威校验入口，手改 JSON 或旧前端写入的错误值同样会被拒绝。

## 三种热重载策略

### preserve_state

```json
"x-hot-reload": "preserve_state"
```

发布新参数值，保留当前 `ctx->state`。适合显示开关、颜色、文案，以及不改变已有累计状态含义的参数。没有显式填写 `x-hot-reload` 时默认使用此策略。

### reset_state

```json
"x-hot-reload": "reset_state"
```

发布新参数值时，在通道处理安全点清空该通道的 logic 状态、旧结果、绘制命令和逻辑画布。下一帧会按新参数重新创建状态。适合状态机规则、累计时长阈值、路径规则等不应继续沿用旧状态的参数。

### restart_required

```json
"x-hot-reload": "restart_required"
```

运行中的进程拒绝包含该参数变化的整次热更新，并继续使用旧运行快照；重启程序后才读取文件中的新值。适合依赖一次性资源初始化或无法安全在线替换的数据结构。

注意：Web 保存成功只表示文件已经写入，不等于 C++ 一定采用了新值。若策略要求重启或校验失败，文件会保留新内容，但当前进程继续使用旧快照；应查看进程日志并在必要时修正配置。

## 热重载实际发生了什么

只有保存“当前进程正在使用的配置文件”才会触发该进程热重载。编辑另存的 `config_xxx.json` 不会影响正在使用另一份配置的进程。

配置监控线程检测文件稳定后执行：

1. 重新解析整份配置；
2. 根据当前 logic 的内嵌 Schema 校验 `logic_parameters`；
3. 拒绝未知键、重复键、错误类型、越界值和未知 logic；
4. 补齐默认值；
5. 按 JSON 语义比较旧值和新值，对象字段顺序变化不算参数变化；
6. 计算所有变化参数中影响最高的热重载策略；
7. 在任何模型或视频流副作用之前拒绝 `restart_required`；
8. 构建包含 ChannelConfig、ROI 和类型化参数表的新不可变快照；
9. 在通道处理安全点原子发布，必要时重置 logic 状态。

因此一次逐帧调用看到的配置、ROI 和模块参数来自同一代快照，不会出现一半新、一半旧。

## Web 为什么不需要为新参数写代码

构建生成器将模块 Schema 投影成 Web 兼容参数元数据，其中生成字段 `storage: "logic_parameters"` 表示该值应放在嵌套对象中。前端通用表单根据类型自动生成：

- 数字输入框；
- 布尔开关；
- 枚举下拉框；
- 单行/多行字符串；
- array/object JSON 编辑框。

保存时 `graphToConfig` 统一写入：

```json
"logic_parameters": {
  "dwell_sec": 8
}
```

重新加载时 `configToGraph` 再还原到该逻辑节点。切换 logic 时，Web 会清掉旧模块的参数键，并使用新模块 Schema 的默认集合，避免把旧逻辑的未知键带给新逻辑。

`logic_path_sop` 的历史 `path_*` 字段和专用流程编辑器暂时保留用于兼容旧配置；以后新增的普通 SOP 扩展参数也应使用本方案，它们会显示在 SOP 面板的“模块扩展参数”区域。

## 构建、生成和部署

### 只校验模块，不编译 C++

```bash
cd /userdata/sop_agent/vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

### 板端 debug 构建

```bash
./build.sh --debug
```

CMake 会重新发现模块 `.cpp/.cc/.cxx`，校验 manifests，并把最小运行时 Schema 编进可执行文件。直接在板端运行该二进制不依赖外部 `logics.json`。

### 正常打包给 Web 使用

```bash
./build.sh my_app_package
```

除嵌入式 Schema 外，正常打包还会自动生成：

```text
my_app_package/logics.json
```

该文件包含 Web 所需的逻辑标签、参数控件、动作和上报字段。它是生成物，不要手工维护。修改源模块时只改对应 `logic.json`。

若手工部署或开发调试 Web，可显式生成到目标 App 根目录：

```bash
python3 scripts/generate_logics_catalog.py \
  --output /目标/App/目录/logics.json
```

可执行文件和 `logics.json` 应来自同一次源码版本并一起部署。如果 Web 清单比正在运行的二进制新，Web 可能允许填写新参数，但旧二进制会把它当作未知键拒绝。

二进制的 `--list-logics` 只用于列出实际编译注册的逻辑名；它不能代替包含完整参数表单信息的 `logics.json`。

## 公共视频流参数和绘制仍然怎么用

模块参数改造没有取消 `ChannelContext` 的公共字段和绘图接口。

```cpp
const int source_width = ctx->src_width;        // 实际解码源宽度
const int source_height = ctx->src_height;      // 首帧前可能为 0
const int logic_width = ctx->frame->cols;       // logic/模型坐标系宽度
const int logic_height = ctx->frame->rows;

const StreamConfig &stream = ctx->config->stream;
const std::string &src_type = stream.src_type;
const std::string &url = stream.url;
const int usb_width = stream.usb_width;
const int usb_height = stream.usb_height;
```

`draw_rect()`、`draw_text()`、`draw_line()`、多边形绘制和 `ctx->display_canvas()` 均继续使用。检测框、ROI 和绘图坐标使用 `ctx->frame` 的模型输入坐标系，不要直接用 `src_width/src_height` 当绘图坐标。完整公共 API 见 [channelcontext-api.md](channelcontext-api.md)。

## 真实参考实现

当前周期截图演示逻辑已经使用这套参数体系：

- Schema：`vision_analysis/src/logic/modules/logic_periodic_snapshot_demo/logic.json`；
- C++ 调用：`vision_analysis/src/logic/modules/logic_periodic_snapshot_demo/logic.cpp`；
- 通用解析器：`vision_analysis/src/logic/core/logic_parameters.h/.cpp`；
- 构建生成器：`vision_analysis/scripts/generate_logics_catalog.py`；
- 热重载入口：`vision_analysis/src/core/app_ctrl.cpp`；
- Web 表单：`web_console/frontend/src/components/NodeConfigPanel.tsx`；
- Web 配置转换：`web_console/frontend/src/utils/configToGraph.ts`、`graphToConfig.ts`。

## 开发检查清单

新增逻辑：

- [ ] 新建 `src/logic/modules/logic_xxx/`；
- [ ] `REGISTER_LOGIC(logic_xxx)` 只传入入口函数，源 `logic.json` 不包含 `name`；
- [ ] `parameters` 是禁止额外键的 object Schema；
- [ ] 每个属性都有正确类型的默认值；
- [ ] 为每个参数选择合理的热重载策略；
- [ ] C++ 使用与 Schema 类型匹配的 `ctx->param_*()`；
- [ ] 运行状态只放 `ctx->state`，不写回参数；
- [ ] 运行 `generate_logics_catalog.py --check`；
- [ ] 重新构建，并把二进制与生成的 `logics.json` 一起部署给 Web。

给现有逻辑增加参数：

- [ ] 只在该模块 `logic.json` 增加属性；
- [ ] 只在该模块 C++ 中调用；
- [ ] 不增加新的 `ChannelConfig`/`REG_C` 中央字段；
- [ ] 不手改生成后的 `logics.json`；
- [ ] 更新二进制和 Web 清单后，再测试参数值热重载。

## 常见问题排查

### Web 看不到新参数

1. 检查实际 App 根目录的 `logics.json` 是否来自最新打包；
2. 检查参数是否被设置了 `x-ui-hidden: true`；
3. 刷新页面或重新打开该 App，使前端重新请求 `/apps/{name}/logics`；
4. 不要只更新二进制而漏掉 `logics.json`。

### 保存后 C++ 没采用新值

1. 确认保存的是进程当前使用的配置文件；
2. 查看日志是否提示类型错误、越界、未知参数或重复键；
3. 查看该参数是否声明为 `restart_required`；
4. 确认运行二进制和 Web `logics.json` 来自同一版本；
5. 配置文件即使保存成功，C++ 拒绝后仍会继续使用旧快照。

### 构建提示 param_* 没有 Schema

检查参数 key 是否完全一致，以及访问器类型是否匹配。参数必须声明在调用它的同一个模块目录的 `logic.json` 中，不能依赖另一个模块的专有 Schema。

### 修改阈值后状态表现不合理

如果旧的累计时长、闩锁或状态机不能继续沿用，应把策略从 `preserve_state` 改为 `reset_state`。不要在 `ctx->state` 中缓存一份参数来绕开热重载。

### 参数属于视频流、模型或显示设置

不要复制成模块参数。先使用 `ctx->config`、`ctx->src_width/height`、`ctx->infer_fps/disp_fps` 等公共接口；只有当前公共上下文确实缺少所有 logic 都需要的运行信息时，才考虑扩展一次公共 `ChannelContext`。
