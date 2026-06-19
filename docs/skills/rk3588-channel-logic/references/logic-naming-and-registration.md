# 逻辑的命名与注册：四个名字的关系 / 网页如何识别 / 失配会怎样

> 回答三个高频问题：
> ① 一个逻辑的 **cpp 文件名 / C++ 函数名 / `REGISTER_LOGIC` 注册的字符串 / `logics.json` 的 `name`**，这几个是什么关系？
> ② 我写的逻辑是怎么**被网页"认出来"**的？
> ③ 这些名字**不一致**会怎样？
>
> 权威源码：`rk3588_yolo/src/logic/channel_logic.h` / `channel_logic.cpp`（分发表、`REGISTER_LOGIC` 宏）、`rk3588_yolo/src/analyzer/channel_pipeline.cpp`（运行时按名取函数）、`web_console/backend/routers/config_io.py`（网页拿到逻辑清单的接口）、`web_console/frontend/src/components/NodeConfigPanel.tsx`（逻辑节点下拉）。

## 一句话结论

**唯一的"身份"是 `REGISTER_LOGIC` 第一个参数那个字符串。** 文件名、C++ 函数名只是给人和编译器看的约定，对外不可见；`config.json` 的 `"logic"` 和 `logics.json` 的 `"name"` 必须**等于这个字符串**，逻辑才能既被网页选到、又能在运行时真正跑起来。

## 四个名字各是什么、谁读它

```
  文件名 logic_xxx.cpp ──(只被 CMake 的 aux_source_directory 收集编译)──> 与"身份"无关
        │ 文件里定义了
        ▼
  C++ 函数名 logic_xxx   (static = 文件内可见，外部根本看不到)
        │ 只被本文件末尾这一行引用：
        ▼
  REGISTER_LOGIC("logic_xxx", logic_xxx)
        │  把【字符串 "logic_xxx"】→【函数指针】登记进分发表(main 之前自动完成)
        ▼
  ┌─────────────────────────────────────────────┐
  │   注册字符串 "logic_xxx"  ← 唯一身份，一切以它为准   │
  └─────────────────────────────────────────────┘
        ▲                                      ▲
  「运行」要对上它                          「网页」要对上它
        │                                      │
  config.json  "logic":"logic_xxx"         logics.json  "name":"logic_xxx"
  决定运行时跑哪个函数                      决定网页能否选到 + 参数怎么渲染
```

| 名字 | 谁读它 | 要和别人一致吗 | 改了/不一致会怎样 |
|---|---|---|---|
| **文件名** `logic_xxx.cpp` | 只有 CMake（决定是否编译进来） | **不必**，纯约定 | 随便起名都能编译运行；约定与逻辑同名只是方便人查找 |
| **C++ 函数名** `logic_xxx` | 只有同文件里的 `REGISTER_LOGIC`（它是 `static`，外部不可见） | **不必** | 改名时同步改 `REGISTER_LOGIC` 第二个参数即可，外部无感 |
| **注册字符串**（`REGISTER_LOGIC` 第 1 参） | 分发表 `channel_logic_get()` 用 `strcmp` 查 | **★核心**，下面两个都要等于它 | 它才是真名 |
| **`logics.json` 的 `name`** | 后端 `/apps/{name}/logics` → 前端下拉 | **必须 == 注册字符串**（也 == `config.json` 的 `logic`） | 见下方《名字不一致会怎样》 |
| （`config.json` 的 `"logic"`） | 运行时拿去查分发表 | **必须 == 注册字符串** | 这是"实际选用了哪个逻辑" |

## 两条互不相干的路

逻辑"能不能跑"和"网页认不认得它"是**两条独立的链**，各看各的来源：

### ① 运行路径（逻辑到底跑不跑）—— 只看 注册字符串 + config.json

`config.json "logic":"logic_xxx"` → `channel_pipeline.cpp` 的 `invoke_channel_logic()` 取出 `logic_name` → `channel_logic_get("logic_xxx")` 在分发表里 `strcmp` → 命中就调那个函数；查不到就返回内部兜底 `logic_null`（什么都不做）。

**这条路跟 `logics.json`、文件名都没关系。** 哪怕 `logics.json` 里没声明，只要 `config.json` 写了正确的注册字符串，逻辑照样跑。

### ② 网页识别路径（能不能在 UI 看到/选到、参数能不能渲染）—— 只看 logics.json

`logics.json` 的 `channel_logics[]` → 后端 `config_io.py` 的 `/apps/{name}/logics` 路由**直接把文件内容透传** → 前端 `api/client.ts` 的 `fetchAppLogics()` 拿去渲染下拉框和参数输入框（`NodeConfigPanel.tsx` 的 `LogicForm`）。

**前端根本不读你的 C++、不知道 `REGISTER_LOGIC` 的存在。** "被网页识别" ≈ "写进了 `logics.json`"。

> **兜底（`--list-logics`）**：只有当该 App 目录**没有** `logics.json` 时，后端才会退一步去跑二进制 `--list-logics`（`config_io.py`），那时列出的才是 C++ 分发表里的注册字符串。正常流程里 **`logics.json` 优先**。

## 网页端：逻辑名只能"选"、不能"手填"（本项目约定）

`NodeConfigPanel.tsx` 的 `LogicForm` 里，「逻辑名称」是一个**下拉框**，选项全部来自 `logics.json`（经 `fetchAppLogics`）。**不再提供任何"自定义名称/手动输入"的文本框**——用户不能给逻辑取一个 `logics.json` 里没有的新名字。

- **为什么这样设计**：手填一个分发表里没有的名字，保存进 `config.json` 后，运行时 `channel_logic_get` 查不到 → 回退 `logic_null` → 该通道**空跑**（表现为"选了逻辑却毫无反应"，且很难一眼看出原因）。把入口收成"只能从 `logics.json` 选"，从源头杜绝这种失配。
- **遗留/未知值怎么显示**：若某通道 `config.json` 里的 `logic` 当前不在 `logics.json`（如老配置、或逻辑已从 `logics.json` 删除），下拉会把它显示成 `xxx（⚠ 不在 logics.json，请重新选择）`，提示用户重选一个合法项，但**不允许**新建名字。
- **全局逻辑（`GlobalLogicNode`）同理**：「逻辑名称」也是纯下拉，选项来自 `known_global_logics`（即 `logics.json` 的 `global_logics`），没有手填入口。

> 推论：**要让一个逻辑能在网页上被选，必须在 `logics.json` 声明它**（光在 C++ 里 `REGISTER_LOGIC` 是不够的——那只保证它"能跑"，不保证"网页选得到"）。

## 名字不一致会怎样（对照表）

| 写法 | 结果 |
|---|---|
| C++ 函数名 ≠ **注册字符串**（第 1 参），如 `REGISTER_LOGIC("logic_banana", logic_apple)` | ✅ 编译、运行都正常。对外的真名是 `"logic_banana"`；`config.json`/`logics.json` 必须填 `logic_banana`，填 `logic_apple` 反而查不到。函数名只是文件内私有标签，易误导，**不建议**。 |
| **第 2 参**（函数指针）写成不存在的函数，如 `REGISTER_LOGIC("logic_x", logic_typo)` | ❌ **编译报错**（未定义标识符）——编译器替你兜底，注册不了空函数。 |
| 同一个 `.cpp` 里对同一个 `func` 写两次 `REGISTER_LOGIC` | ❌ 编译报错（宏生成的注册器变量 `_logic_reg_<func>` 重定义）。 |
| 两个**不同文件**注册了相同的**字符串** | ⚠ 不报错，但运行时**后注册的覆盖先注册的**（`register_logic` 同名则覆盖），只有一个生效。 |
| `config.json` 的 `logic` 指向一个**未注册**（或拼错）的名字 | ⚠ 运行时 `channel_logic_get` 查不到 → 回退 `logic_null` → 该通道**什么都不跑**。网页下拉会显示成 `⚠ 不在 logics.json`。 |
| 逻辑**注册了但 `logics.json` 没声明** | ✅ 运行能跑（只要 `config.json` 名字对），但**网页下拉里看不到、参数也不渲染**。 |

## `REGISTER_LOGIC` 宏背后做了什么

宏定义（`channel_logic.h`）：

```cpp
struct LogicRegistrar {
    LogicRegistrar(const char *name, ChannelLogicFunc func) { register_logic(name, func); }
};
#define REGISTER_LOGIC(name_str, func) \
    static const LogicRegistrar _logic_reg_##func(name_str, func)
```

`REGISTER_LOGIC("logic_banana", logic_apple)` 展开成：

```cpp
static const LogicRegistrar _logic_reg_logic_apple("logic_banana", logic_apple);
//                                                  ↑注册表 key       ↑函数指针
```

这个文件作用域的静态对象，会在 `main()` 之前（静态初始化阶段）构造，构造时执行 `register_logic("logic_banana", logic_apple)`，把**字符串 → 函数指针**塞进分发表。于是：

- 注册表的 **key 是 `name_str` 那个字符串**，不是函数名。
- 函数名 `func` 只出现在两处：作为真正要指向的函数、被 `##` 拼进注册器变量名 `_logic_reg_##func`。它**从不进入注册表的 key**，所以对运行时/网页都不可见。

## 实践建议：五处保持一致

技术上只有后三者（注册字符串、`logics.json` 的 `name`、`config.json` 的 `logic`）**必须**相等；但维护时请让**五处全一致**，省得给自己挖坑：

```
文件名 logic_xxx.cpp  ==  函数名 logic_xxx  ==  REGISTER_LOGIC("logic_xxx", ...)
                       ==  logics.json 的 "name":"logic_xxx"  ==  config.json 的 "logic":"logic_xxx"
```

全局逻辑（`global_xxx`）同理：`global_logic.cpp` 里 `register_global_logic("global_xxx", global_xxx)` 的**字符串** == `logics.json` 的 `global_logics[].name` == `config.json` 的 `global_logics[].logic`。

> 这条"全一致"约定，正是网页端把「逻辑名称」收成"只能从 `logics.json` 下拉选、不能手填"的原因——从 UI 入口就保证了 `config.json` 里的名字一定是个合法、可跑、可识别的注册名。
