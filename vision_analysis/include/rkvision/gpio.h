/**
 * @file gpio.h
 * @brief 视觉主程序 GPIO 公共接口（libgpiod 1.x）
 *
 * 日常业务逻辑通常只需要下面三个函数，不需要理解后面的查询结构体：
 *
 *   gpio_set_output("GPIO6_A2", 1);       // 输出高电平
 *   gpio_set_input("GPIO6_B3");           // 切换成输入
 *   gpio_read_input("GPIO6_B3", &value);  // 读取逻辑 0/1
 *
 * GPIOPinInfo_t、GPIOVoltageInfo_t、GPIOOutputStatus_t 主要供设备管理页面、
 * 硬件排查和诊断使用。它们不是控制 GPIO 所必需的参数。
 *
 * 所有函数均线程安全，使用 "GPIOx_Yz" 命名：
 * x 是当前系统的 gpiochip 编号，Y 是组内 bank 字母，z 是 bank 内编号。
 *
 * 注意：GPIO 只能读取逻辑 0/1。GPIOVoltageInfo_t 描述的是芯片资料或
 * 设备树给出的标称/可能电压，不是对引脚电压的实测。
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
     * 启动时批量配置一条引脚。
     *
     * 这是【输入结构体】，三个字段由调用者填写。例如：
     *   { "GPIO6_A2", DIR_OUTPUT, 0 }
     */
    typedef struct
    {
        char pinName[GPIO_TEXT_SHORT]; /* 要控制的引脚名，例如 "GPIO6_A2" */
        int direction;                 /* DIR_INPUT=输入，DIR_OUTPUT=输出 */
        int val;                       /* 输出初始值：0=低，非0=高；输入时忽略 */
    } GPIOCfg_t;

    /**
     * 引脚的标称高电平信息。
     *
     * 这是【输出结构体】，由 gpio_get_voltage_info() 填写。它描述芯片资料
     * 或设备树中的供电信息，不是万用表/ADC 实测值。
     */
    typedef struct
    {
        /* IO 所属电源域，例如 "VCCIO5"、"PMUIO1" 或 "PCA9555供电"。 */
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

        /*
         * 内核给 GPIO 控制器实例的标签。例如：
         *   "gpio0"  = RK3588 内部第 0 组 GPIO；
         *   "3-0021" = I²C 3 号总线上地址 0x21 的扩展芯片。
         * 它用于定位硬件实例，通常不直接传给控制函数。
         */
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
        int onlyAvailable; /* 1=隐藏已被占用的引脚；0=显示全部 */
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

        int hasRuntimeRecord; /* 1=存在旧后台运行记录 */
        int runtimeValue;     /* 旧记录值，不保证仍等于当前硬件电平 */

        int hasSavedValue; /* 1=存在下次开机恢复值 */
        int savedValue;    /* 下次开机要恢复的值，不代表当前电平 */

        int persistenceEnabled; /* 1=电平持久化开关已开启 */
    } GPIOOutputStatus_t;

    /**
     * 【什么时候调用】
     * 在程序启动阶段、GPIO 工作线程启动之前调用一次，用于批量声明本程序准备
     * 使用的输入/输出引脚，并提前检查名称、方向和占用情况。例如：
     *
     *   static const GPIOCfg_t gpioList[] = {
     *       { "GPIO6_A2", DIR_OUTPUT, 0 }, // 输出，首次默认低电平
     *       { "GPIO6_B3", DIR_INPUT,  0 }, // 输入，val 字段被忽略
     *   };
     *   gpio_init(gpioList, 2);
     *
     * 【是否必须】
     * 不是必须。只控制少量引脚时，可以不调用本函数，直接调用
     * gpio_set_input() / gpio_set_output()；引脚会在首次使用时自动加入管理。
     *
     * 【调用后的动作】
     * 输出引脚会立即设为输出并设置明确的初始电平；持久化开启且已经保存过
     * 输出值时，优先恢复保存值。输入引脚会立即切换成输入并由后台持续持有。
     *
     * 【不要这样用】
     * 不要逐帧调用，不要在多个工作线程运行期间反复调用。再次调用会释放本进程
     * 直接持有的旧线路、清空内部引脚表，并按新数组重新配置全部引脚。
     *
     * @param cfg  GPIOCfg_t 数组。
     * @param size 数组元素数量。
     * @return 0=配置数组已处理；-1=参数无效。单个引脚失败会打印日志，
     *         但不会阻止其他引脚初始化。
     */
    int gpio_init(const GPIOCfg_t cfg[], int size);

    /**
     * 【什么时候调用】
     * 程序准备退出时调用一次。必须先停止并等待所有可能调用 GPIO 的工作线程，
     * 然后再调用本函数。例如：
     *
     *   stop_and_join_gpio_users();
     *   gpio_deinit();
     *
     * 【它会做什么】
     * 释放本进程直接申请的 GPIO 线路，关闭本进程打开的 gpiochip，并清空内部
     * 引脚记录。只做信息查询且从未控制 GPIO 的短测试程序可以不调用。
     *
     * 【它不会做什么】
     * 不会自动把输出拉低，不会删除持久化状态，也不会释放/停止独立 GPIO 后台
     * 已经持有的线路。因此后台输出会继续保持最后一次电平。
     *
     * 如果设备退出时必须关闭继电器或喇叭，应先明确设置安全电平，再释放：
     *
     *   gpio_set_output("GPIO6_A2", 0);
     *   gpio_deinit();
     *
     * 【不要这样用】
     * 不要在其他线程仍可能调用 GPIO 时执行，否则那些线程随后会重新打开线路，
     * 或与退出清理产生时序冲突。
     */
    void gpio_deinit(void);

    /**
     * 将一个引脚切换为输入。
     *
     * @param pinName 引脚名，例如 "GPIO6_B3"。
     * @return 0=成功；负数=失败，可查看 errno 和日志。
     *
     * 持久化开启时会清除这个引脚以前保存的输出值，防止重启后又变回输出。
     */
    int gpio_set_input(const char *pinName);

    /**
     * 将一个引脚切换为输出并立即设置电平。
     *
     * @param pinName     引脚名，例如 "GPIO6_A2"。
     * @param initialValue 0=低电平，非0=高电平。
     * @return 0=成功；负数=失败，可查看 errno 和日志。
     */
    int gpio_set_output(const char *pinName, int initialValue);

    /**
     * 严格读取外部输入电平，不会自动改变引脚方向。
     *
     * @param pinName 引脚名，例如 "GPIO6_B3"。
     * @param value   成功时写入 0（低）或 1（高）。
     * @return 0=成功；-1=失败。线路是输出或被占用时 errno 通常为 EBUSY。
     */
    int gpio_read_input(const char *pinName, int *value);

    /**
     * 查询输出状态，不申请线路、不改变方向。
     *
     * @param pinName 引脚名。
     * @param status  函数填写的输出结构体。
     * @return 0=查到至少一种状态；-1=无状态或查询失败。
     */
    int gpio_get_output_status(const char *pinName, GPIOOutputStatus_t *status);

    /**
     * 查询一条线路的完整诊断信息，不改变 GPIO。
     *
     * @param pinName 引脚名。
     * @param info    函数填写的输出结构体。日常优先查看 direction、access、
     *                voltage.description；其他字段主要用于硬件排查。
     * @return 0=成功；-1=失败。
     */
    int gpio_get_pin_info(const char *pinName, GPIOPinInfo_t *info);

    /**
     * 查询标称高电平信息，不代表实测电压，也不会翻转引脚。
     *
     * @return 0=成功；-1=失败。即使成功，description 也可能为“供电未描述*”。
     */
    int gpio_get_voltage_info(const char *pinName, GPIOVoltageInfo_t *info);

    /**
     * 无扰动枚举 GPIO。
     *
     * 第一次传 items=NULL、capacity=0，只取得需要的数量；第二次分配数组后
     * 再取得每条 GPIOPinInfo_t。普通控制逻辑不需要调用本函数。
     *
     * @param options      筛选条件；传 NULL 表示全部 GPIO。
     * @param items        调用方提供的结果数组；第一次查询数量时传 NULL。
     * @param capacity     items 能容纳的元素个数。
     * @param requiredCount 返回符合条件的总条数。
     * @return 0=成功；-1=失败。
     */
    int gpio_list_lines(const GPIOListOptions_t *options, GPIOPinInfo_t *items, size_t capacity, size_t *requiredCount);

    /**
     * 按保存文件恢复全部输出，主要供系统启动恢复流程使用。
     *
     * @param restoredCount 可为 NULL；返回成功数量。
     * @param failedCount   可为 NULL；返回失败数量。
     * @return 0=全部成功或持久化未开启；-1=至少一项失败。
     */
    int gpio_restore_outputs(int *restoredCount, int *failedCount);

    /* ------------------------------------------------------------------
     * 兼容旧逻辑的便捷接口。新代码优先使用上面语义明确的 gpio_* 接口。
     * ------------------------------------------------------------------ */

    /** 旧名称：设置输出电平，等价于 gpio_set_output(pinName, val)。 */
    int pin_out_val(const char *pinName, int val);

    /** 旧便捷名称：输出高电平。 */
    int pin_set_high(const char *pinName);

    /** 旧便捷名称：输出低电平。 */
    int pin_set_low(const char *pinName);

    /**
     * 兼容读取：输入时读输入，输出时可读回输出。严格输入采样请使用
     * gpio_read_input()。
     */
    int read_pin_val(const char *pinName);

#ifdef __cplusplus
}
#endif
