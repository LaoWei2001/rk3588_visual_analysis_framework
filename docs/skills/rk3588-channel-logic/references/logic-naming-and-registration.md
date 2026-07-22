# 通道逻辑的函数名、注册和 Web 清单

本文说明当前模块化目录下，一个 channel logic 的身份如何在 C++、配置和 Web 之间保持一致。

## 当前唯一真源

每种通道逻辑位于一个独立模块目录：

```text
rk3588_yolo/src/logic/modules/<module_dir>/
├── logic.cpp
├── logic.json
└── ...
```

当前的唯一 logic ID 来自 C++ 函数标识符：

```text
static void logic_xxx(ChannelContext *ctx)
    ↓ REGISTER_LOGIC(logic_xxx) 自动字符串化
    ↓ 生成 logics.json channel_logics[].name = "logic_xxx"
    ↓ config.json channels[].logic = "logic_xxx"
```

`REGISTER_LOGIC` 只接受一个参数，它既是实际函数，也是生成给 config、Web 和外部 API 的名称。模块目录只用于组织源码，可以与 logic ID 不同，但建议保持一致以便查找。

```cpp
static void logic_xxx(ChannelContext *ctx)
{
    // ...
}

REGISTER_LOGIC(logic_xxx);
```

不要再另外手写注册字符串。

## logic.json 和生成的 logics.json 不是同一个文件

### 模块 logic.json：源码

模块自己的 `logic.json` 是除 logic ID 外的元数据真源，包含：

- `label`；
- `parameters` 参数 Schema；
- `actions`；
- `report_fields` / `business_fields`；
- 其他该模块 Web 所需元数据。

源文件中不允许手写 `name`；构建器会从 `REGISTER_LOGIC(func)` 取函数名并注入生成的 `logics.json`。新增或修改逻辑时也不手工维护中央 `channel_logics` 数组。

### App 根目录 logics.json：生成物

正常打包时，生成器聚合：

```text
src/logic/modules/*/logic.json
+ src/logic/catalog.json
→ App 根目录 logics.json
```

Web 后端 `/apps/{name}/logics` 读取这份生成物，前端据此显示逻辑下拉、参数控件、按钮和上报字段。

不要直接编辑生成后的 `logics.json`；下一次打包会覆盖它，而且手改会造成 Web 清单与 C++ 二进制不一致。

## 两条运行链路

### C++ 运行链路

```text
channels[].logic
  → 配置加载时查找二进制内嵌模块 Schema
  → 校验该 logic 的 logic_parameters
  → 运行快照保存 logic 名和类型化参数
  → channel_logic_get(name)
  → 调用 REGISTER_LOGIC 注册的函数
```

当前配置加载阶段已经要求 logic 存在于内嵌 Schema。名字未知时，启动配置会失败；热重载时则拒绝新配置并继续使用旧运行快照，不再把拼错名称当作正常逻辑静默运行。

### Web 识别链路

```text
App 根目录 logics.json
  → Web 后端 /apps/{name}/logics
  → 前端 LogicForm
  → 逻辑下拉、参数、动作和上报字段
```

前端不扫描 C++ 源码。要获得完整 Web 表单，实际 App 目录必须部署与二进制同版本的生成 `logics.json`。

如果 App 根目录没有可解析的 `logics.json`，后端会执行二进制 `--list-logics`，得到实际注册的逻辑名字作为降级选项；该命令只返回名字，不含参数、动作和字段元数据，因此不能代替完整清单。

如果 `logics.json` 和二进制探测都不可用，接口返回空的 `channel_logics` 和明确错误，不使用硬编码逻辑名兜底，避免已删除模块重新出现在 Web 下拉框中。

## 构建期身份校验

生成器会检查：

- 每个 `modules/*/` 下都有 `logic.json`；
- 源 `logic.json` 不包含 `name`；
- 每个模块恰好存在一次 `REGISTER_LOGIC(func)`；
- 模块若注册 action handler，`REGISTER_LOGIC_ACTION(logic_func, handler)` 的第一参数引用同一个 logic 函数且不重复；
- 不同模块的 logic 函数名不重复；
- 参数 Schema 合法；
- 字符串字面量形式的 `ctx->param_*()` 键存在且类型匹配。

可以不编译 C++，单独执行：

```bash
cd /userdata/sop_agent/rk3588_yolo
python3 scripts/generate_logics_catalog.py --check
```

`build.sh --debug` 和正常打包也会自动执行同一套校验。

## 新增 logic 的最小步骤

1. 新建 `src/logic/modules/logic_xxx/`；
2. 新建 `logic.cpp`，包含 `logic/core/logic_common.h`；
3. 实现 `logic_xxx(ChannelContext*)`；
4. 用 `REGISTER_LOGIC(logic_xxx)` 注册；
5. 新建同目录 `logic.json`，只声明 `label`、参数、动作和上报字段，不写 `name`；
6. 在 `parameters` 中声明该模块专有参数；
7. 运行生成器检查并重新构建；
8. Web 场景部署新二进制和同次打包生成的 `logics.json`；
9. 在通道配置或 Web 下拉中选择 `logic_xxx`。

CMake 会递归收集 `src/logic` 下的 `.cpp/.cc/.cxx`。使用 `build.sh` 会重新运行 CMake，因此新增源文件不需要手改中央源文件列表。

## 名字不一致时会发生什么

| 问题 | 结果 |
|---|---|
| 模块目录缺少 `logic.json` | 生成器报错，构建停止 |
| 源 `logic.json` 仍手写 `name` | 生成器报错，提示由 `REGISTER_LOGIC(func)` 生成 |
| 同一模块重复注册 | 生成器报错，构建停止 |
| 不同模块使用相同的 logic 函数名 | 生成器报错，构建停止 |
| `param_*()` 字面量键不存在或访问器类型不匹配 | 生成器报错，构建停止 |
| 配置中的 `logic` 未被当前二进制编译 | 启动失败，或热重载被拒绝并保留旧快照 |
| Web `logics.json` 比二进制新 | Web 可能显示新 logic/参数，但旧 C++ 会拒绝未知内容 |
| 二进制比 Web `logics.json` 新 | 逻辑能运行，但 Web 可能看不到新逻辑或新参数 |

部署时把二进制和生成清单视为一个版本对，不能只更新其中一个。

## channel logic 与 global logic 的区别

本文只针对 `REGISTER_LOGIC` 通道逻辑。global logic 当前使用 `global_logic.cpp` 中的显式注册和 `global.global_logics` 配置，发现链路与模块化 channel logic 不同；新增全局逻辑应参考 `docs/skills/rk3588-global-logic/SKILL.md`。
