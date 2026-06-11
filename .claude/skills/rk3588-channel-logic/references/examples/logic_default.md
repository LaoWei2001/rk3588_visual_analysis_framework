# logic_default — 默认（无动作）

- **上报**：无
- **可调参数**：无
- **用到的能力**：无（占位/直通）

## 做什么
什么都不做的占位逻辑。通道选它时只走推理 + 通用显示（检测框由显示层画），不附加任何业务判断。新建通道、或临时关掉某通道的业务逻辑时用。

## 完整实现
```cpp
static void logic_default(ChannelContext *ctx)
{
    (void)ctx;
}
```

## 接线
- 注册（channel_logic.cpp `channel_logic_init()`）：`register_logic("logic_default", logic_default);`
- logics.json：`{ "name": "logic_default", "label": "默认（无动作）", "params": [] }`

## 复用提示
新写一个逻辑时，可以从这个空壳起步，逐步往里加判断/绘制/上报。
