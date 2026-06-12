# 给某个逻辑加一个可配置参数（代码 + 热重载 + 网页可配）

当某个 `logic_xxx` 需要一个用户能调的数值/开关（半径、停留秒数、阈值、开关…），按本文做。项目用一套**基于偏移量的配置注册表**（`config_registry`），把"加字段"变成"填三处对齐 + 一行 REG_C"，**热重载和网页可配几乎是自动实现的**。

## 全景：一个参数从代码到网页的一条链

```
config.h: ChannelConfig 加字段 dwell_sec ──┐
config_init.cpp: REG_C("dwell_sec",INT,..) ─┤  注册表(g_cfg_reg)知道 "dwell_sec"→ChannelConfig 偏移+类型
logic 里读 ctx->config->dwell_sec ──────────┘
                                             │
logics.json: params 加 {key:"dwell_sec",..} ─→ 网页逻辑节点渲染出一个输入框
                                             │
用户在网页改 → graphToConfig 写进该通道 config.json → 保存
                                             │
config_monitor 线程检测 config.json 变化 → sync_fields 把新值热拷进运行配置 → 下一帧 ctx->config->dwell_sec 即新值
```

## 唯一要记的规则：四处 key 必须完全一致

**① ChannelConfig 字段名 == ② REG_C 的键 == ③ logics.json 的 `param.key` == ④ 逻辑里 `ctx->config->key` 读的名字。** 四处对齐，整条链就通了。logics.json 顶部 `_comment` 就是这条规则。

---

## Step 1 — 在 ChannelConfig 加字段（`rk3588_yolo/src/config/config.h`）

给一个**默认值**（用户没配时用它，也是热重载/解析失败时的兜底）：

```cpp
struct ChannelConfig {
    ...
    int dwell_sec = 3;        // 停留报警秒数
};
```

## Step 2 — 注册到配置表（`rk3588_yolo/src/config/config_init.cpp`）

一行 `REG_C(键, 类型, 字段)`。`REG_C` 展开成 `g_cfg_reg.add_channel("dwell_sec", ConfigType::INT, offsetof(ChannelConfig, dwell_sec))`——它告诉解析器"JSON 里 `dwell_sec` 这个键，对应 ChannelConfig 的这个偏移、这个类型"。**注册一次，初始解析和热重载都自动认它**，你不用手写任何 cJSON 解析。

```cpp
REG_C("dwell_sec", INT, dwell_sec);
```

类型（`ConfigType`，定义在 `config_registry.h`）：

| REG_C 类型       | C++ 字段类型                   | logics.json `type`                  |
| -------------- | -------------------------- | ----------------------------------- |
| `INT`          | `int`                      | `"int"`                             |
| `FLOAT`        | `float`                    | `"float"`                           |
| `BOOL`         | `bool`                     | `"bool"`                            |
| `STRING`       | `std::string`              | `"string"` / `"text"`（多行）/ `"enum"` |
| `STRING_ARRAY` | `std::vector<std::string>` | （类别列表等，前端按需处理）                      |

## Step 3 — 在逻辑里读它

```cpp
int dwell = ctx->config ? ctx->config->dwell_sec : 3;   // 永远从 ctx->config 现读，别缓存（见"热重载"）
```

## Step 4 — 声明到 logics.json，让网页能配（`rk3588_yolo/src/logic/logics.json`）

在该 logic 的 `params` 数组里加一条。**不加这条，网页就渲染不出这个输入框**（但参数本身仍可用，只是不能在 UI 改）。

```json
{ "name": "logic_dwell_alarm", "label": "停留报警", "report": "server", "params": [
    { "key": "dwell_sec", "type": "int", "label": "停留秒数", "default": 3, "min": 1, "max": 60,
      "help": "目标在 ROI 内连续停留超过此秒数即报警" }
] }
```

`param` 字段：

| 字段                     | 必填      | 说明                                              |
| ---------------------- | ------- | ----------------------------------------------- |
| `key`                  | ✓       | **必须等于 ChannelConfig 字段名**（四处对齐）                |
| `type`                 | ✓       | `int`/`float`/`string`/`bool`/`enum`/`text`（多行） |
| `label`                |         | 网页显示的中文名（缺省用 key）                               |
| `default`              |         | 默认值（切到该 logic 时自动填）                             |
| `min` / `max`          |         | 数字范围（int/float）                                 |
| `options`              | enum 必填 | 下拉选项数组，如 `["low","high"]`                       |
| `help` / `placeholder` |         | 提示文字                                            |

**网页这边怎么生效的（机制，便于排查）**：编辑器逻辑节点(`NodeConfigPanel` 的 `LogicForm`)调 `/apps/{name}/logics` 拿到 logics.json → 按 `param.type` 动态渲染控件（int/float→数字框，bool→勾选，enum→下拉，text→多行）→ 用户改的值存在逻辑节点上 → 保存时 `graphToConfig` 把这些参数写进该通道的 `config.json` → 重载回显由 `configToGraph` 还原。**前端不需要为新参数改任何代码**，加一条 logics.json 声明即可。

---

## 每通道一份：同名不同值，谁都能读但读到的是自己那份

- **字段定义一次，值每通道独立**：`AppConfig` 里是 `std::vector<ChannelConfig> channels`，每通道一个 `ChannelConfig`。你在 `config.h` 写**一次** `dwell_sec`，物理上 `channels[0].dwell_sec`、`channels[1].dwell_sec` 就是两块内存——**同名、各通道一份独立的值**。每通道的值来自它自己 config.json 的设置；没设的通道用结构体默认值。
- **任何 logic 都能读 `ctx->config->dwell_sec`，但读到的恒为"本通道那一份"**：字段在每个通道的 config 上都存在，所以哪怕某 logic 用不上也能合法读取；但 `ctx->config` 只指向本通道——通道 1 读 `ctx->config->dwell_sec` 拿到的是通道 1 自己的值（没配就是默认值），**不是某个"专门用它的通道"的值**。`ctx->config` **没有**读别的通道配置字段的能力。想跨通道共享同一个值，要么每个通道 config.json 都写，要么做成全局字段（`AppConfig` 全局 + 加载时下发到各通道）。
- **现成例子**：`radius`（`config.h`）只被 `logic_hook`/`logic_roll` 用，却是**每个通道都有**的字段；别的通道/逻辑读 `ctx->config->radius` 只会拿到默认值、用不上而已——定义一次、个别逻辑用、其余通道默认值闲置，完全正常，不浪费也不串台。

### 配置参数 vs 运行时状态：变量 A 该放哪？

| A 的性质 | 放哪 | 读写 |
|---|---|---|
| 用户设的**只读**参数（半径 / 阈值 / 停留秒数…） | `ChannelConfig` 字段，读 `ctx->config->A` | 只读（`ctx->config` 是 `const`，**别往里写**） |
| 逻辑运行中**自己更新**的状态（计数 / 计时 / 闩锁 / 去重表…） | `ctx->state`（每通道一格 `shared_ptr`） | 可读可写 |

> 把"逻辑要改的值"放进 ChannelConfig 是错的：① `ctx->config` 只读；② 热重载 `sync_fields` 会用 config.json 的值把它**覆盖回去**。两者都天然每通道独立，按"只读配置 / 可写状态"二选一，且都不要用 `static`。

---

## 热重载：为什么改了值不用重启（这是关键，也几乎免费）

`config_monitor_thread_func`（`rk3588_yolo/src/core/app_ctrl.cpp`）是一条常驻线程，轮询 `config.json` 的 mtime；文件变化并稳定后，重新解析成 `new_cfg`，然后在**写锁内**调：

```cpp
g_cfg_reg.sync_fields(&ctrl->config.channels[i], &new_cfg.channels[i], false);
```

`sync_fields` 会把**注册表里所有 REG_C 字段**的新值，按偏移逐个拷进**正在运行的** `ctrl->config.channels[i]`。所以：

> **只要你用 REG_C 注册了字段，它就自动参与热重载。** 用户在网页改 `dwell_sec` 保存 → config.json 变 → 监控线程 sync_fields → 你的逻辑**下一帧** `ctx->config->dwell_sec` 读到的就是新值。你**不需要写任何 reload 回调**。

### 唯一要遵守的读取约定

**每帧从 `ctx->config->key` 现读**，不要在逻辑外/`ctx->state` 里缓存一份参数值——否则热重载更新了 `ctx->config`，你读的还是旧缓存。把参数当"每帧问一次配置"来用。

### 哪些不走这套自动热重载（例外，写参数时要知道）

| 改了什么                                       | 怎么热重载的                                             |
| ------------------------------------------ | -------------------------------------------------- |
| REG_C 注册的普通参数（你加的就是这类）                     | **自动**，sync_fields 热拷，下一帧生效                        |
| `model_path`/`model_type`/`label_path`     | 监控线程检测到 → `algorithm_reload_channel_model` 热换模型    |
| `obj_thresh`/`nms_thresh`/`detect_classes` | 热更新到推理引擎                                           |
| 通道的 `logic` 名                              | 热切换逻辑函数 + 重置该通道 logic_state                        |
| `stream`（src_type/url/device/usb_width…）   | 不在注册表，监控线程单独比对、必要时重启解码器                            |
| **ROI（roi_zones.json）**                    | **不热重载**——只在程序启动 `load_roi_zones` 时读。改 ROI 必须停止再启动 |

---

## 完整示例：给 logic_dwell_alarm 加"停留秒数"

1. `config.h`：`int dwell_sec = 3;`
2. `config_init.cpp`：`REG_C("dwell_sec", INT, dwell_sec);`
3. 逻辑里：`int dwell = ctx->config ? ctx->config->dwell_sec : 3;`（每帧读）
4. `logics.json`：在 `logic_dwell_alarm` 的 `params` 加上面那条 `dwell_sec` 声明。
5. 编译装包 → 网页该通道逻辑节点就多出"停留秒数"输入框；改它保存，运行中即时生效，无需重启。

## 坑

- **key 不对齐**：四处任意一处拼错，要么网页改了 C++ 读不到，要么解析不出——先核对四处字符串完全一致。
- **缓存了参数值**：在逻辑里把 `ctx->config->dwell_sec` 存进 `static` 或 `ctx->state` 只读一次 → 热重载失效。每帧现读。
- **忘了 logics.json**：参数能用、能热重载，但网页上没有它的输入框（只能手改 config.json）。要网页可配就必须加声明。
- **类型不匹配**：REG_C 用 `INT` 但 logics.json 写 `"type":"float"`，会出现取整/精度问题——保持一致。
- **新参数没默认值**：ChannelConfig 字段一定给默认值，避免老配置（没这个键）解析后是随机值。
