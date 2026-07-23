# logic_button_demo — Web 按钮加减数字演示

- 源码：`vision_analysis/src/logic/modules/logic_button_demo/logic.cpp`
- 按钮清单：`vision_analysis/src/logic/modules/logic_button_demo/logic.json`
- 注册：`REGISTER_LOGIC` + `REGISTER_LOGIC_ACTION`

这是仓库中最简单的 Web 业务按钮示例。画面初始显示数字 `1`，实时画面的通道控制区显示两个按钮：

- `+1`：发送 `increment` 动作，数字增加 1；
- `-1`：发送 `decrement` 动作，数字减少 1，最小保持为 1。

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
  }
]
```

## C++ 处理

按钮 ID 会原样成为 `action.name`。处理器使用基础 C++ 字符串比较判断动作并修改当前通道的状态：

```cpp
if (action.name == "increment")
    state->number += 1;

if (action.name == "decrement" && state->number > 1)
    state->number -= 1;
```

逐帧函数再通过 `draw_text()` 把 `state->number` 画到视频画面中央。动作处理器在当帧逐帧函数之前执行，因此按钮被消费后，新数字会直接出现在该通道的下一次逻辑画面中。

`ButtonDemoState` 保存在 `ctx->state` 中，所以多个通道同时使用本逻辑时，每个通道分别维护自己的数字。按钮声明、Socket/FIFO 链路和 handler 约束详见 `../custom-button-actions.md`。
