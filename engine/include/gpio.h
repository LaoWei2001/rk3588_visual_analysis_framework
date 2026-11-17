/** @file gpio.h
 * @brief 设置和读取引脚的高低电平，用于按钮、指示灯、继电器等设备。
 * 常用顺序：gpio_set_input() 后 gpio_read_input() 读输入；gpio_set_output() 直接设置输出。
 * 函数可直接调用，无需另写初始化代码；引脚名可用 gpio_list_lines() 查询。
 * 下方的 GPIO6_A2/GPIO6_B3 是示例名称，请换成设备上实际需要使用的引脚。
 * 查询函数会填写对应结构体；调用前自己声明变量，再把变量地址传进去。
 * 示例中的 printf/perror 用于打印结果，使用时另行包含 <stdio.h>。
 * GPIO 只读逻辑 0/1，电压查询给出的是资料中的电压说明，不是实际测量结果。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DIR_OUTPUT 1
#define DIR_INPUT 0
#define GPIO_DIRECTION_UNKNOWN (-1)

#define GPIO_ACCESS_AVAILABLE 0
#define GPIO_ACCESS_FRAMEWORK 1
#define GPIO_ACCESS_BUSY 2

/* 查询结果中文字字段的内部容量。调用者通常不需要使用这两个宏。 */
#define GPIO_TEXT_SHORT 32
#define GPIO_TEXT_LONG 96

    /**
     * 引脚的标称高电平信息。
     *
     * 这是【输出结构体】，由 gpio_get_voltage_info() 填写。它描述芯片资料
     * 或设备树中的供电信息，不是万用表/ADC 实测值。
     */
    typedef struct
    {
        /* 给这组引脚供电的位置名称，仅作查询说明，例如 "VCCIO5"。 */
        char powerDomain[GPIO_TEXT_LONG];

        /* 给人看的结论，例如 "约1.8V（固定）"、"1.8V或3.3V*"。 */
        char description[GPIO_TEXT_LONG];

        /* 标称下限，单位微伏。1800000 表示 1.8V；0 表示无法确定。 */
        int minimumMicrovolts;

        /* 标称上限，单位微伏。3300000 表示 3.3V；0 表示无法确定。 */
        int maximumMicrovolts;

        /* 1=资料给出固定标称值；0=只有范围或电压未知。仍不代表实测精度。 */
        int isExact;

        /* 是否来自 ADC/电压计实测。GPIO 无此能力，所以当前永远为 0。 */
        int isMeasured;
    } GPIOVoltageInfo_t;

    /**
     * 一条 GPIO 的完整诊断信息。
     *
     * 这是【输出结构体】，由 gpio_get_pin_info() 或 gpio_list_lines() 填写。
     * 日常控制只使用 pinName 即可；其余字段用于回答“它属于哪个芯片、为什么
     * 不能控制、当前被谁占用、标称电压是多少”等排查问题。
     *
     * 以本机 GPIO6_B3 为例：
     *   pinName="GPIO6_B3", chipName="gpiochip6", chipLabel="3-0021",
     *   controller="nxp,pca9555", group=6, offset=11。
     */
    typedef struct
    {
        /* 对外使用的引脚名；控制函数传这个值，例如 "GPIO6_B3"。 */
        char pinName[GPIO_TEXT_SHORT];

        /* Linux GPIO 字符设备名，例如 "gpiochip6"，对应 /dev/gpiochip6。 */
        char chipName[GPIO_TEXT_SHORT];

        /* 芯片报告的标签，例如 "gpio0" 或 "3-0021"，只用于查看和排查；控制引脚时仍传 pinName。 */
        char chipLabel[GPIO_TEXT_LONG];

        /*
         * 动态识别出的控制器型号/驱动，例如 "nxp,pca9555"、
         * "rockchip,rk3588/gpio0"。用来判断 GPIO 来自哪颗芯片。
         */
        char controller[GPIO_TEXT_LONG];

        /*
         * 设备树为这条线声明的固定用途名称，例如 "usb_host_pwren"。
         * 空字符串表示设备树没有给它命名，不等于线路不存在。
         */
        char lineName[GPIO_TEXT_LONG];

        /*
         * 当前正在占用线路的内核驱动或进程，例如 "usb_host_pwren"、
         * "rk3588_gpio_control"。空字符串表示当前没有使用者。
         * lineName 是静态用途名，consumer 是运行时占用者，两者含义不同。
         */
        char consumer[GPIO_TEXT_LONG];

        /* gpiochip 编号。例如 GPIO6_B3 的 group=6。 */
        unsigned int group;

        /* 芯片内线路编号。例如 B3 = 8*1+3，所以 offset=11。 */
        unsigned int offset;

        /* 当前方向：DIR_INPUT、DIR_OUTPUT；无法判断时为 -1。 */
        int direction;

        /* 1=当前被某个驱动/进程占用，0=当前空闲。 */
        int used;

        /* 1=占用者是本视觉框架，0=空闲或由其他功能占用。 */
        int frameworkOwned;

        /*
         * 最推荐查看的综合结论：
         *   GPIO_ACCESS_AVAILABLE = 空闲候选；
         *   GPIO_ACCESS_FRAMEWORK = 已由本框架控制；
         *   GPIO_ACCESS_BUSY      = 被其他功能占用，不应操作。
         */
        int access;

        /* 这条线路的标称高电平信息；不是实际电压测量结果。 */
        GPIOVoltageInfo_t voltage;
    } GPIOPinInfo_t;

    /**
     * gpio_list_lines() 的筛选条件。这是【输入结构体】，由调用者填写。
     */
    typedef struct
    {
        int chip;          /* -1=全部；6=只查看 /dev/gpiochip6 */
        int onlyAvailable; /* 1=隐藏被其他功能占用的引脚，本程序正在用的仍列出；0=显示全部 */
    } GPIOListOptions_t;

    /**
     * 一个输出引脚可能存在三种不同来源的值。
     *
     * 这是【输出结构体】，由 gpio_get_output_status() 填写。每个 value 只有
     * 对应的 has*Value/has*Record 为 1 时才有效。
     */
    typedef struct
    {
        int hasCurrentValue; /* 1=已确认当前正作为输出保持 */
        int currentValue;    /* 当前输出：0=低，1=高 */

        int hasRuntimeRecord; /* 1=存在以前操作引脚时的记录 */
        int runtimeValue;     /* 运行记录值，不保证仍等于当前硬件电平 */

        int hasSavedValue; /* 1=存在下次开机恢复值 */
        int savedValue;    /* 下次开机要恢复的值，不代表当前电平 */

        int persistenceEnabled; /* 1=开启了保存输出电平、重启后恢复的功能 */
    } GPIOOutputStatus_t;

    /** @brief 把引脚设为输入，用它读取按钮、开关或传感器的高低电平。
     * @param pinName 设备上的引脚名称，例如 "GPIO6_A2"；请从实际引脚列表中选取。
     * @return 0 表示成功；负数表示失败，原因可用 perror() 打印。
     * 返回 -3 表示引脚已经切成输入，但设置保存失败；开启重启恢复时，会清除该引脚以前的输出恢复值。
     * 示例：
     * @code
     * int level = 0;
     * if (gpio_set_input("GPIO6_B3") == 0 && gpio_read_input("GPIO6_B3", &level) == 0)
     * {
     *     printf("输入电平：%d\n", level);
     * }
     * @endcode
     */
    int gpio_set_input(const char *pinName);

    /** @brief 把引脚设为输出并设置高低电平，用于控制指示灯、继电器等设备。
     * @param pinName 设备上的引脚名称，例如 "GPIO6_A2"；请从实际引脚列表中选取。
     * @param initialValue 0 为低电平，1 为高电平；其他非 0 值也会按高电平处理。
     * @return 0 表示成功；负数表示失败。-3 表示电平已设置，但用于恢复的设置没有保存成功。
     * 无需提前初始化引脚。高电平是否让设备动作取决于接线和设备类型；只在电平需要改变时调用即可。
     * 示例：
     * @code
     * int result = gpio_set_output("GPIO6_A2", 1); // 设置为高电平
     * if (result != 0)
     * {
     *     perror("设置输出失败");
     * }
     * @endcode
     */
    int gpio_set_output(const char *pinName, int initialValue);

    /** @brief 读取输入引脚当前是高电平还是低电平，用来判断按钮或外部信号。
     * @param pinName 设备上的引脚名称，例如 "GPIO6_A2"；请从实际引脚列表中选取。
     * @param value 接收电平的 int 变量地址，例如 &level；成功后 0 为低电平，1 为高电平。
     * @return 0 表示读取成功，可以使用 value；-1 表示失败，此时不要使用 value 判断电平。
     * 引脚需要先设为输入；如果仍是输出或被其他功能占用，读取会失败，本函数不会替你切换方向。
     * 示例：
     * @code
     * int level = 0;
     * if (gpio_read_input("GPIO6_B3", &level) == 0)
     * {
     *     printf("当前是%s电平\n", level ? "高" : "低");
     * }
     * @endcode
     */
    int gpio_read_input(const char *pinName, int *value);

    /** @brief 查询输出引脚当前的电平，以及以前记录或准备重启恢复的值。
     * @param pinName 设备上的引脚名称，例如 "GPIO6_A2"；请从实际引脚列表中选取。
     * @param status 接收查询结果的变量地址，例如 &status；不要传空指针。
     * @return 0 表示至少查到一种记录，-1 表示查询失败；成功也不保证查到了当前电平。
     * hasCurrentValue 为 1 才能读 currentValue；hasRuntimeRecord 对应以前的 runtimeValue，
     * hasSavedValue 对应重启恢复值 savedValue，后二者不能当成当前电平。查询不会改变引脚。
     * 示例：
     * @code
     * GPIOOutputStatus_t status;
     * if (gpio_get_output_status("GPIO6_A2", &status) == 0 && status.hasCurrentValue)
     * {
     *     printf("当前输出电平：%d\n", status.currentValue);
     * }
     * @endcode
     */
    int gpio_get_output_status(const char *pinName, GPIOOutputStatus_t *status);

    /** @brief 查看引脚是输入还是输出、是否被占用等信息，帮助选择引脚或排查操作失败。
     * @param pinName 设备上的引脚名称，例如 "GPIO6_A2"；请从实际引脚列表中选取。
     * @param info 接收信息的 GPIOPinInfo_t 变量地址，例如 &info。
     * @return 0 表示成功，可以读取 info；-1 表示失败，原因可用 perror() 打印。
     * info.access：GPIO_ACCESS_AVAILABLE 表示空闲，GPIO_ACCESS_FRAMEWORK 表示本程序正在用，
     * GPIO_ACCESS_BUSY 表示被其他功能占用。
     * 这只是查询时的状态，不会替你占住引脚；后续设置或读取时仍要检查返回值。
     * 示例：
     * @code
     * GPIOPinInfo_t info;
     * if (gpio_get_pin_info("GPIO6_A2", &info) == 0)
     * {
     *     printf("引脚：%s，使用状态：%d\n", info.pinName, info.access);
     * }
     * @endcode
     */
    int gpio_get_pin_info(const char *pinName, GPIOPinInfo_t *info);

    /** @brief 查询资料中说明的引脚高电平电压，例如 1.8V 或 3.3V。
     * @param pinName 设备上的引脚名称，例如 "GPIO6_A2"；请从实际引脚列表中选取。
     * @param info 接收电压说明的 GPIOVoltageInfo_t 变量地址；一般先看 description。
     * @return 0 表示查询完成，-1 表示失败；查询完成也可能表示资料里没有明确电压。
     * 这不是对当前电压的测量，也不会改变引脚。minimumMicrovolts/maximumMicrovolts 用微伏表示，1800000 即 1.8V。
     * 示例：
     * @code
     * GPIOVoltageInfo_t info;
     * if (gpio_get_voltage_info("GPIO6_A2", &info) == 0)
     * {
     *     printf("电压说明：%s\n", info.description);
     * }
     * @endcode
     */
    int gpio_get_voltage_info(const char *pinName, GPIOVoltageInfo_t *info);

    /** @brief 列出设备上的引脚及其状态，方便找到可用的引脚名称。
     * @param options 筛选条件；NULL 查全部，chip=-1 选全部芯片，onlyAvailable=1 隐藏被其他功能占用的引脚。
     * @param items 接收信息的数组；只问数量时可传 NULL，并把 capacity 填 0。
     * @param capacity 数组最多能放几项，不是字节数，例如 pins[16] 填 16。
     * @param requiredCount 接收符合条件的总数量，传 size_t 变量的地址，不能为 NULL。
     * @return 0 表示成功，-1 表示失败；实际写入项数是 capacity 与 *requiredCount 中较小的一个。
     * 数组太小不会越界，但结果会不完整；可先查询数量、再分配数组后重新查询。两次查询间数量可能变化。
     * 示例：
     * @code
     * GPIOPinInfo_t pins[16];
     * size_t total = 0;
     * if (gpio_list_lines(NULL, pins, 16, &total) == 0)
     * {
     *     size_t written = total < 16 ? total : 16;
     *     for (size_t i = 0; i < written; ++i)
     *     {
     *         printf("%s\n", pins[i].pinName);
     *     }
     * }
     * @endcode
     */
    int gpio_list_lines(const GPIOListOptions_t *options, GPIOPinInfo_t *items, size_t capacity, size_t *requiredCount);

#ifdef __cplusplus
}
#endif
