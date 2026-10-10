#pragma once

#include <gpio.h>

/* Engine-owned GPIO lifecycle. Call only before/after logic worker threads. */
#ifdef __cplusplus
extern "C"
{
#endif

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
     * 按保存文件恢复全部输出，主要供系统启动恢复流程使用。
     *
     * @param restoredCount 可为 NULL；返回成功数量。
     * @param failedCount   可为 NULL；返回失败数量。
     * @return 0=全部成功或持久化未开启；-1=至少一项失败。
     */
    int gpio_restore_outputs(int *restoredCount, int *failedCount);

#ifdef __cplusplus
}
#endif
