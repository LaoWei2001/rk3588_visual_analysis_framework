# Logic 模块目录

通道逻辑和全局逻辑都按“每种逻辑一个模块目录”组织：

```text
src/logic/
├── core/                       # 注册表、ChannelContext、全局逻辑框架
├── modules/
│   └── logic_xxx/
│       ├── logic.cpp           # 入口、动作处理和 REGISTER_LOGIC
│       ├── logic.json          # 参数、动作、上报字段等模块元数据
│       └── ...                 # 复杂逻辑可继续添加 state/engine/render 等文件
├── global_modules/
│   └── global_xxx/
│       ├── logic.cpp           # 入口和 REGISTER_GLOBAL_LOGIC
│       ├── logic.json          # 参数 Schema 和 Web 元数据
│       └── ...
└── catalog.json                # 模型类型等非模块共享能力
```

这里的一个模块对应一种逻辑类型，不对应某个实际视频通道。多个通道可以同时使用同一个
`logic_xxx` 模块，各通道的运行状态仍由框架独立管理。

通道配置中的 `logic` 是可选后处理步骤。省略或留空时，框架不调用任何业务模块，但视频
仍正常显示，模型检测结果仍由通用渲染链路绘制。包括 `logic_default` 在内的所有模块都是
普通可删除目录；系统不依赖某个模块充当空逻辑兜底。

## 新增通道逻辑

1. 新建 `modules/logic_xxx/`（目录名建议与入口函数同名，但外部 logic ID 不从目录名取）。
2. 在 `logic.cpp` 中包含 `logic/core/logic_common.h`，实现逻辑入口，并通过
   `REGISTER_LOGIC(logic_xxx)` 注册。宏会把函数名自动转为 config/Web/外部 API 的 logic ID。
3. 新建 `logic.json`，声明 `label`、参数、动作和上报字段；不要手写 `name`。
4. 重新运行 CMake 和构建脚本。CMake 会递归收集模块源码，打包脚本会聚合模块清单。

`logic.json` 只描述该逻辑支持的参数、动作和上报字段，不保存某个通道的运行时参数值。
实际通道配置仍保存在 `assets/config*.json`。

## 新增模块专有参数

普通逻辑参数只修改本模块的两个文件，不再修改 `ChannelConfig`、`config_init.cpp`、
`app_ctrl.cpp` 或前端字段列表。

在 `logic.json` 的 `parameters.properties` 中声明参数：

```json
{
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "dwell_sec": {
        "type": "number",
        "title": "滞留时间",
        "description": "达到该时长后报警",
        "minimum": 0.5,
        "maximum": 300,
        "default": 5,
        "x-unit": "秒",
        "x-step": 0.5,
        "x-hot-reload": "reset_state"
      }
    }
  }
}
```

在同目录的 `logic.cpp` 中直接读取：

```cpp
const float dwell_sec = ctx->param_float("dwell_sec");
```

可用访问器：

- `param_float()`：JSON Schema `number`；
- `param_int()`：`integer`；
- `param_bool()`：`boolean`；
- `param_string()`：`string`；
- `param_json()`：`array` / `object` 的规范 JSON 文本；
- `has_param()`：检查键是否存在（Schema 默认值会在启动时自动补齐）。

实际配置统一保存在通道的嵌套对象中：

```json
{
  "logic": "logic_xxx",
  "logic_parameters": {
    "dwell_sec": 8
  }
}
```

`x-hot-reload` 支持三种策略：

- `preserve_state`：发布新值，保留 `ctx->state`；
- `reset_state`：发布新值时安全清空当前通道逻辑状态；
- `restart_required`：运行中拒绝该次变更，要求重启。

Schema 会在构建时嵌入可执行文件。配置在启动和热重载时解析一次并转成不可变类型化
参数表，逐帧逻辑不会解析 JSON。类型错误、越界值和未知键会使新配置被拒绝，旧运行
快照继续工作。

`logic_path_sop` 的完整流程也遵守同一规则，唯一持久化入口是
`logic_parameters.flow`。SOP 子画布直接读写这个 object；步骤、边、入口、出口、阈值和
子画布坐标不再压缩成通道顶层 CSV 字段。`flow` 使用 `reset_state` 热更新策略，图发生变化
时框架会清空旧状态，logic 在新状态第一次运行时解析一次结构化对象。

可在不构建 C++ 的情况下单独校验所有模块：

```bash
python3 scripts/generate_logics_catalog.py --check
```

CMake 编译（包括 `build.sh --debug`）会生成并编译嵌入式能力清单。正常打包还会生成
应用根目录的 `logics.json`，供后端和前端动态渲染。生成文件不应人工修改；需要调整
某种逻辑时，只修改对应模块内的 `logic.json`。

## 新增全局逻辑

1. 新建 `global_modules/global_xxx/logic.cpp` 和 `logic.json`。
2. 实现 `static void global_xxx(GlobalContext *gctx)`，末尾写
   `REGISTER_GLOBAL_LOGIC(global_xxx)`；函数名是唯一全局 logic ID。
3. 在模块 `logic.json.parameters.properties` 声明专有参数，通过
   `gctx->param_float/int/bool/string/json()` 读取。
4. 在 `global.global_logics[]` 配置实例；参数值放在该实例的
   `logic_parameters` 对象中。

全局模块清单和参数表会与通道模块一起聚合进 `logics.json`。Web 直接读取当前 App 的
`global_logics`，不维护全局 logic 硬编码列表。每个全局实例使用稳定 `instance_id`；参数
热更新遵守 Schema 的 `preserve_state`、`reset_state`、`restart_required`，只影响发生变化的实例。

画布配置两路视频、发布通道变量、在全局节点聚合并复用统一上报链路的完整示例，见
`modules/logic_global_input_demo/` 和 `global_modules/global_channel_aggregate_demo/`。

## 框架与业务边界

- `core/` 只存放所有逻辑共享的生命周期、上下文、注册和绘制接口。
- `modules/logic_xxx/` 只存放该逻辑自己的代码和元数据。
- `global_modules/global_xxx/` 只存放该跨通道逻辑自己的代码和元数据。
- 小逻辑可以只有一个 `logic.cpp`；只有文件确实变大时才继续按状态、算法、上报、绘制拆分。
- 模块之间不要通过相对路径包含彼此的内部头文件；需要复用的稳定能力应提升到明确的公共层。
