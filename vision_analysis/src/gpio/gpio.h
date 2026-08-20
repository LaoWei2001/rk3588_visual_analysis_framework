/**
 * @file gpio.h
 * @brief GPIO 底层控制封装（基于 libgpiod 1.x）
 *
 * 能力来源: 移植自 Easy EAI 08_GPIO 例程 (commonApi/gpio.c), 并做了以下增强:
 *   - 线程安全: 内部全局互斥锁, 多通道逻辑并行调用安全
 *   - 懒加载:   未预注册的引脚首次调用时自动打开, 通道逻辑里可直接使用
 *   - 错误缓存: 引脚名错误/被占用等同类错误只打印一次, 不会逐帧刷屏
 *   - 修复原版 gpio_uinit() 中 pinObj 可能为空的野指针问题
 *
 * 引脚命名规则与 08_GPIO 例程完全一致: "GPIOx_Yz"
 *   x = GPIO 组号(0~6), Y = 组内 bank 字母(A~Z), z = bank 内编号(0~9)
 *   例如 "GPIO6_A0" -> gpiochip6 上的 0 号线; "GPIO6_B3" -> 8*1+3 = 11 号线
 *
 * 两种使用方式:
 *   1. 【懒加载】通道逻辑里直接调用, 零配置:
 *        pin_set_high("GPIO6_A2");   // 拉高(如继电器吸合)
 *        pin_set_low ("GPIO6_A2");   // 拉低
 *        首次调用自动按输出方向打开该引脚并缓存, 之后复用句柄。
 *   2. 【预注册】在 main.cpp 启动阶段声明一次, 启动即校验(引脚名/占用/方向):
 *        // { 引脚名, 方向, 初始值 }
 *        static const GPIOCfg_t gpio_cfgs[] = {
 *            { "GPIO6_A2", DIR_OUTPUT, 0 },
 *        };
 *        gpio_init(gpio_cfgs, ARRAY_SIZE(gpio_cfgs));
 *      预注册后同样可以随时 pin_* 调用; 未预注册的引脚仍走懒加载。
 *
 * 方向语义:
 *   - pin_out_val / pin_set_high / pin_set_low: 引脚按【输出】使用。
 *     懒加载时自动按输出打开; 若已预注册为输入, 调用会失败并打印错误。
 *   - read_pin_val: 引脚按【输入】使用。
 *     懒加载时自动按输入打开; 已预注册为输出时仍可读取当前输出值。
 *   - 同一引脚既当输入又当输出时, 请用 gpio_init 预注册为对应方向,
 *     或按输出预注册后直接 read_pin_val(读回的是输出值)。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIR_OUTPUT 1
#define DIR_INPUT  0

/** 引脚配置项(供 gpio_init 预注册使用) */
typedef struct {
    char pinName[32]; /* "GPIOx_Yz" 格式, 见文件头命名规则 */
    int  direction;   /* DIR_OUTPUT 或 DIR_INPUT */
    int  val;         /* 打开时的初始值(输出方向生效; 输入方向忽略) */
} GPIOCfg_t;

/**
 * 预注册一批引脚并打开(可选, 推荐在启动阶段调用一次)。
 * 调用后内部引脚表会整体重建: 已懒加载的引脚句柄会被释放并按新配置重新打开。
 * 同一引脚重复注册以最后一次为准。
 *
 * @param cfg  引脚配置数组
 * @param size 数组元素个数
 * @return 0 成功; -1 数组非法或内存不足(单个引脚打开失败只打印, 不影响整体返回)
 */
int gpio_init(const GPIOCfg_t cfg[], int size);

/** 释放全部引脚句柄并清空引脚表(程序退出时调用)。 */
void gpio_deinit(void);

/**
 * 设置指定引脚的输出电平(线程安全, 可逐帧调用)。
 * 引脚未注册时自动按【输出】方向懒加载打开。
 *
 * @param pinName "GPIOx_Yz" 格式引脚名
 * @param val      0=低电平, 非0=高电平
 * @return 0 成功; -1 引脚名非法; -2 打开/设置失败(错误只打印一次)
 */
int pin_out_val(const char *pinName, int val);

/** 便捷函数: 拉高指定引脚(等价 pin_out_val(name, 1))。 */
int pin_set_high(const char *pinName);

/** 便捷函数: 拉低指定引脚(等价 pin_out_val(name, 0))。 */
int pin_set_low(const char *pinName);

/**
 * 读取指定引脚的当前电平(线程安全)。
 * 引脚未注册时自动按【输入】方向懒加载打开。
 *
 * @param pinName "GPIOx_Yz" 格式引脚名
 * @return 0/1 电平值; 失败返回 -1
 */
int read_pin_val(const char *pinName);

#ifdef __cplusplus
}
#endif
