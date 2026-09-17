# GPIO 公共接口

## 先看这里：普通逻辑只需要三个函数

如果只是控制继电器、读取开关量，不需要理解 GPIOPinInfo_t，也不用填写
chipName、chipLabel、controller 等字段。直接这样调用：

    // GPIO6_A2 切换成输出并拉高
    gpio_set_output("GPIO6_A2", 1);

    // GPIO6_A2 保持输出方向并拉低
    gpio_set_output("GPIO6_A2", 0);

    // GPIO6_B3 切换成输入，然后读取外部高低电平
    gpio_set_input("GPIO6_B3");
    int value = 0;
    gpio_read_input("GPIO6_B3", &value);

只有在制作 GPIO 管理页面，或者排查“某条线路属于哪颗芯片、被谁占用、为何不能
控制”时，才需要 GPIOPinInfo_t 等查询结构体。

视觉主程序通过 gpio.h / gpio.cpp 集中提供与 rk3588-gpioctl 一致的核心能力。GPIO 实时后台
运行时，方向、电平、运行记录和开机保存值均由后台在一个串行请求内统一提交；业务模块仍然
只调用原有函数，不需要处理锁、状态文件或协议细节：

- 动态切换输入或输出方向；
- 设置、保持并读取数字电平；
- 严格区分“读取输入”和“查询输出状态”；
- 查询当前输出、运行记录和开机保存值；
- 无扰动枚举全部 GPIO，查看方向、线路名、占用者和可控状态；
- 查询 RK3588 IO 电源域或设备树提供的标称高电平信息；
- 按持久状态恢复全部输出；
- 兼容旧版 GPIO 后台和原有 pin_* 调用。

接口定义全部位于 gpio.h，实现按“控制、状态、枚举/电压”分区集中在
gpio.cpp。主程序已链接 libgpiod，逻辑模块只需包含 gpio/gpio.h。

## 方向与电平

输出方向必须给出明确初始电平，避免切换方向时出现不确定脉冲：

    gpio_set_output("GPIO6_A2", 0);
    gpio_set_output("GPIO6_A2", 1);

切换为输入后再严格采样：

    if (gpio_set_input("GPIO6_B3") == 0) {
        int value = 0;
        if (gpio_read_input("GPIO6_B3", &value) == 0) {
            // value 为逻辑 0 或 1
        }
    }

gpio_read_input() 只允许读取输入线路。如果目标正在输出或被其他驱动占用，
函数返回 -1，并设置 errno（常见为 EBUSY），不会偷偷改变方向。

正式部署也可以启动时批量预注册：

    static const GPIOCfg_t gpio_cfgs[] = {
        { "GPIO6_A2", DIR_OUTPUT, 0 },
        { "GPIO6_B3", DIR_INPUT,  0 },
    };
    gpio_init(gpio_cfgs, sizeof(gpio_cfgs) / sizeof(gpio_cfgs[0]));

实时控制服务运行时，输入和输出均由后台持续持有；服务未运行或是旧版本时，
主程序安全回退到本地 libgpiod 控制。主程序退出路径会调用 gpio_deinit()。

## 输出状态

状态查询不会申请线路，也不会改变方向：

    GPIOOutputStatus_t status{};
    if (gpio_get_output_status("GPIO6_A2", &status) == 0) {
        if (status.hasCurrentValue)
            printf("当前输出=%d\n", status.currentValue);
        if (status.hasSavedValue)
            printf("开机保存值=%d\n", status.savedValue);
    }

三个来源含义不同：

- currentValue：后台或本进程确认当前正在保持的输出；
- runtimeValue：兼容旧后台的运行记录，不能单独保证仍是硬件现状；
- savedValue：下次开机恢复使用的持久值，不代表当前电平。

每个值都有对应的 has 标志。只有 hasCurrentValue=1 时 currentValue 才有效；
只有 hasSavedValue=1 时 savedValue 才有效。这样可以区分“值刚好是 0”和
“根本没有这个值”。

## 枚举 GPIO

gpio_list_lines() 采用两次调用模式，避免固定数组长度：

    GPIOListOptions_t options{-1, 1}; // 全部 chip，仅空闲/本框架持有
    size_t count = 0;
    gpio_list_lines(&options, nullptr, 0, &count);
    std::vector<GPIOPinInfo_t> lines(count);
    gpio_list_lines(&options, lines.data(), lines.size(), &count);

指定 options.chip = 6 可只查询 gpiochip6；onlyAvailable = 0 返回包括内核占用
线路在内的全部结果。枚举只读取线路信息，不申请 GPIO、不翻转电平。

单个引脚可直接查询：

    GPIOPinInfo_t info{};
    gpio_get_pin_info("GPIO6_B3", &info);

GPIOPinInfo_t 是查询结果，不是控制参数。调用前只需创建一个空变量，函数会把所有
字段填好。结构体中的 char xxx[...] 都是用于保存文字的字符数组，不是让调用者自己
计算或拼接的。

### GPIOPinInfo_t 字段到底是什么

以当前板卡的 GPIO6_B3 为例：

| 字段 | 本机示例 | 含义 | 日常是否需要 |
| --- | --- | --- | --- |
| pinName | GPIO6_B3 | 对外控制使用的引脚名，传给 gpio_set_input/output | 是 |
| chipName | gpiochip6 | Linux 设备名，对应 /dev/gpiochip6 | 否，诊断用 |
| chipLabel | 3-0021 | 控制器实例标签，表示 I²C 3 号总线、地址 0x21 | 否，诊断用 |
| controller | nxp,pca9555 | 控制器芯片型号；说明它来自 PCA9555 扩展芯片 | 否，诊断用 |
| lineName | 空 | 设备树预先声明的线路用途；没有命名时为空 | 否，诊断用 |
| consumer | 空 | 当前占用线路的驱动/进程；为空表示没有使用者 | 排查占用时看 |
| group | 6 | gpiochip 编号，也就是 GPIO6_B3 中的 6 | 通常不需要 |
| offset | 11 | 芯片内序号；B3 = 8×1+3 = 11 | 通常不需要 |
| direction | DIR_INPUT | 当前是输入还是输出 | 查询时常用 |
| used | 0 | 是否已经被某个程序或驱动占用 | 查询时常用 |
| frameworkOwned | 0 | 占用者是否为本视觉框架 | 查询时常用 |
| access | GPIO_ACCESS_AVAILABLE | 综合后的“空闲/本框架/其他占用”结论 | 最推荐看 |
| voltage | 供电未描述* | 标称高电平和电源域信息，不是实测值 | 接线前查看 |

最容易混淆的是以下四项：

- chipName 是 Linux 创建设备时使用的名字；
- chipLabel 用来区分同一种控制器的不同硬件实例；
- controller 表示它实际是哪一种控制器芯片；
- lineName 是线路预设用途，consumer 是当前真正占用它的人。

例如一条 USB 供电控制线可能有 lineName=usb_host_pwren，同时
consumer=usb_host_pwren，说明这条线已经被 USB 驱动使用，业务逻辑不应再控制。

info.access 的取值为：

- GPIO_ACCESS_AVAILABLE：当前空闲；
- GPIO_ACCESS_FRAMEWORK：已由本框架持有；
- GPIO_ACCESS_BUSY：其他内核功能或进程占用，不应强制操作。

“空闲”只说明软件没有占用，不保证引脚已引出或电气上适合翻转，使用前仍需核对
板卡原理图。

## 电压信息的边界

    GPIOVoltageInfo_t voltage{};
    gpio_get_voltage_info("GPIO6_B3", &voltage);

返回的是 RK3588 数据手册/IO 电源域或设备树稳压器描述推导出的标称信息：

- minimumMicrovolts / maximumMicrovolts 为 0 表示未知；
- isExact=1 表示设备资料或设备树给出固定标称值；
- isMeasured 当前始终为 0。

GPIO 字符设备只能读取逻辑 0/1，不能测出 2.87V 之类的实际电压。需要实测时，
必须通过分压和保护电路接入 ADC，或增加 I²C/SPI 电压监测芯片，再使用相应驱动读取。

## 持久化与恢复

电平保持开关开启时：

- 每次成功设置输出，后台都会在同一请求内更新 /var/lib/rk3588-gpio/GPIOx_Yz.state；
- 切换成输入时，后台会在同一请求内删除该引脚旧的输出恢复值；
- gpio_restore_outputs() 可以恢复全部已保存输出。

保持开关关闭时仍可实时控制 GPIO，但不会读取、写入或删除原有开机恢复值。
对 `gpio_set_output()`、`pin_out_val()`、`gpio_set_input()` 的调用方式没有变化；新版封装会优先
使用统一状态协议，并在连接旧后台时自动进入兼容路径。

## 兼容接口

| 兼容函数 | 等价/语义 |
| --- | --- |
| pin_out_val(name, value) | gpio_set_output(name, value) |
| pin_set_high(name) | 输出高电平 |
| pin_set_low(name) | 输出低电平 |
| read_pin_val(name) | 兼容读取，输出线路也可能读回输出值 |

新代码读取外部输入时应使用 gpio_read_input()，不要用语义较宽的 read_pin_val()。
