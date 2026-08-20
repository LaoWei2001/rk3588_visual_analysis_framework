# GPIO 底层控制

基于 libgpiod 1.x 的引脚控制封装，移植自 Easy EAI 08_GPIO 例程并做了线程安全、
懒加载、错误缓存等增强。供通道逻辑 / 全局逻辑随时调用，用于控制继电器、指示灯等外设。

## 引脚命名

与 08_GPIO 例程一致，格式 `GPIOx_Yz`：

| 段   | 含义                     | 取值       |
| ---- | ------------------------ | ---------- |
| x    | GPIO 组号（gpiochip 编号） | 0 ~ 6      |
| Y    | 组内 bank 字母           | A ~ Z      |
| z    | bank 内编号              | 0 ~ 9      |

例如 `GPIO6_A0` → gpiochip6 上的 0 号线；`GPIO6_B3` → 8×1+3 = 11 号线。

## 两种使用方式

### 1. 懒加载（零配置，推荐先这样用）

通道逻辑里直接调用即可，引脚首次使用时自动按调用方向打开：

```cpp
#include "gpio/gpio.h"

static void my_logic(ChannelContext *ctx)
{
    // 检测到目标 -> 继电器吸合; 否则断开
    if (ctx->has_target("person")) {
        pin_set_high("GPIO6_A2");
    } else {
        pin_set_low("GPIO6_A2");
    }
}
REGISTER_LOGIC(my_logic);
```

- `pin_out_val / pin_set_high / pin_set_low` → 引脚按【输出】使用
- `read_pin_val` → 引脚按【输入】使用
- 引脚名写错 / 被占用时错误只打印一次，之后每次调用会自动重试

### 2. 预注册（启动即校验，建议正式使用时加上）

在 main.cpp 启动阶段声明一次，引脚名、占用情况在启动时就能暴露：

```cpp
#include "gpio/gpio.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
// 结构体成员顺序: { 引脚名, 方向, 初始值 }
static const GPIOCfg_t gpio_cfgs[] = {
    { "GPIO6_A2", DIR_OUTPUT, 0 },  // 继电器
    { "GPIO6_A1", DIR_INPUT,  0 },  // 外部信号输入
};

// 与其他子系统 _init 一起调用
gpio_init(gpio_cfgs, ARRAY_SIZE(gpio_cfgs));
```

预注册后同样可随时 `pin_*` 调用；未预注册的引脚仍走懒加载，两者可以混用。

## API 一览

| 函数                              | 说明                                   |
| --------------------------------- | -------------------------------------- |
| `gpio_init(cfg[], size)`          | 预注册一批引脚（可选）                 |
| `gpio_deinit()`                   | 释放全部引脚与芯片句柄（退出时调用）   |
| `pin_out_val(name, val)`          | 设置输出电平，val=0 低 / 非 0 高       |
| `pin_set_high(name)`              | 拉高（等价 `pin_out_val(name, 1)`）    |
| `pin_set_low(name)`               | 拉低（等价 `pin_out_val(name, 0)`）    |
| `read_pin_val(name)`              | 读取电平，返回 0/1，失败 -1            |

## 注意事项

- 全部接口线程安全，多通道逻辑并行调用互不干扰。
- 预注册为 `DIR_INPUT` 的引脚调用 `pin_out_val` 会失败并打印错误（尊重配置）。
- 懒加载为输入的引脚改调输出时会自动重新以输出方向打开。
- 建议在 main.cpp 退出路径上调用 `gpio_deinit()`（与其它 `_deinit` 并列）。

## 演示模块

见 `src/logic/modules/logic_course_gpio/`：检测到指定类别目标时驱动输出引脚。
