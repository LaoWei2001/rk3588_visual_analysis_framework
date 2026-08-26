# 通道模块 `logic.json`

生成器以每个 `vision_analysis/src/logic/modules/<目录>/logic.json` 和同目录 C++ 注册宏共同生成
`logics.json`。源码 manifest 是真源，App 根目录中的聚合文件是构建产物，不应手改。

## 最小结构

```json
{
  "label": "人员计数",
  "event_types": [],
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {}
  },
  "report_fields": []
}
```

不要写顶层 `name`：生成器从唯一的 `REGISTER_LOGIC(func)` 得到 ID。不要写旧式顶层 `params`。
每个模块必须恰好注册一个通道 logic。

## 参数 Schema

`parameters.type` 必须是 `object`，`additionalProperties` 必须是 `false`，每个 property 必须有
支持的 `type` 和类型匹配的 `default`。当前类型：`string`、`number`、`integer`、`boolean`、
`array`、`object`。

可用元数据包括 `title`、`description`、`minimum`、`maximum`、字符串 `enum`、`x-placeholder`、
正数 `x-step`、`x-unit`、`x-ui-hidden`，以及字符串 textarea 的 `x-widget: "textarea"`。

`x-hot-reload` 当前取值：

| 值 | 语义 |
|---|---|
| `preserve_state` | 默认值；热更新参数并保留模块状态 |
| `reset_state` | 热更新参数并重置模块状态 |
| `restart_required` | 该参数变化时拒绝本次热更新，要求重启 |

生成器会静态检查字面量 `param_*()` 调用是否有对应 property，并检查访问方法与 Schema 类型匹配。

## outputs

通道向全局 logic 发布的数据写在 `outputs[]`：

```json
"outputs": [
  {
    "key": "person_count",
    "type": "integer",
    "label": "当前人数",
    "help": "本业务帧内的完整人数"
  }
]
```

类型仅为 `string`、`number`、`integer`、`boolean`、`json`。生成器会检查字面量
`publish_string/number/int/bool/json()` 的 key 和类型。没有 outputs 的模块可以省略该数组。

## 事件与字段

`event_types` 是必填数组；无事件时也写 `[]`。事件项需要唯一 `id`，可附 `label`、`help`。
只要 C++ 调用 `report_event()`，数组就不能空；C++ 中字面量 `request.event_type` 必须已声明。

`report_fields[]` 声明 `event_field()`/`event_json_field()` 产生的动态字段，字段类型仅支持
`string`、`number`、`boolean`、`json`。当前生成器同时检查：C++ 使用的字面量 key 不得漏声明，
manifest 声明的 key 也必须确实由 C++ 赋值。

模块自带接口模板时，以相对路径列入 `report_templates[]`。文件必须位于本模块目录内，模板的
`owner_logic` 必须等于注册 ID。构建会把这些模板生成到 App 的 `report_templates/`；不要直接改
生成目录。

## Actions

`actions[]` 每项使用唯一 `id`，通常还包含 `label`、`style`、`help`。当前校验要求：

- manifest 有非空 `actions` 时，C++ 必须有一个匹配注册 ID 的 `REGISTER_LOGIC_ACTION`；
- C++ 注册了 Action handler 时，manifest 也必须有非空 `actions`；
- Action 的处理细节见 [通道 Action](actions.md)。

## 其他元数据

`business_fields[]` 如使用，以唯一 `path` 标识。它是供业务/模板选择的元数据，不能代替
`report_fields` 或 outputs。复制现有 manifest 前先确认目标模块确实消费这些字段。

## 校验与生成

只校验源码清单：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

常规构建会生成并打包 catalog；源码改动应提交 `logic.cpp`、`logic.json` 和模块自带模板，而不是
提交对运行时生成清单的手工修补。
