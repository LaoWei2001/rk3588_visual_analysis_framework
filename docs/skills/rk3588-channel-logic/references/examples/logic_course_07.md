# logic_course_07 — Web 按钮和每通道状态演示

- 源码：`vision_analysis/src/logic/modules/logic_course_07/logic.cpp`
- 按钮清单：`vision_analysis/src/logic/modules/logic_course_07/logic.json`
- 注册：`REGISTER_LOGIC` + `REGISTER_LOGIC_ACTION`

这是当前仓库的 Web 业务按钮入门示例。它在画面右侧显示两个数字，实时画面的通道控制区显示三个按钮：

- `+1`：发送 `increment`，当前选中的数字增加 1；
- `-1`：发送 `decrement`，当前选中的数字减少 1；
- “切换数字”：发送 `change`，切换当前操作的数字。

## 按钮声明

按钮由模块 `logic.json` 的 `actions[]` 提供给 Web：

```json
"actions": [
  {
    "id": "increment",
    "label": "+1",
    "style": "primary"
  },
  {
    "id": "decrement",
    "label": "-1",
    "style": "default"
  },
  {
    "id": "change",
    "label": "切换数字",
    "style": "default"
  }
]
```

## C++ 处理

按钮 ID 会原样成为 `action->name`。处理器接收只读 `ChannelAction` 指针，检查非空后使用基础 C++ 字符串比较判断动作并修改当前通道的状态：

```cpp
if (action->name == "increment")
    state->number[state->num] += 1;

if (action->name == "decrement")
    state->number[state->num] -= 1;

if (action->name == "change")
    state->num = !state->num;
```

逐帧函数再通过 `draw_text()` 显示两个数字，并用颜色标出当前选择。动作处理器在当帧逐帧函数之前执行，因此按钮被消费后，新状态会直接出现在该通道的下一次逻辑画面中。

`ButtonDemoState` 保存在 `ctx->state` 中，所以多个通道同时使用本逻辑时，每个通道分别维护自己的数字。按钮声明、HTTP → Unix Socket → 通道动作队列链路和 handler 约束详见 `../custom-button-actions.md`。

当前项目有意把 HTTP 动作端点设计为免登录接口，方便教学和设备联动。logic 仍需严格校验
action 名称与 payload；端口可达范围由部署环境控制。
