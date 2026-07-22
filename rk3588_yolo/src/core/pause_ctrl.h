/**
 * @file pause_ctrl.h
 * @brief 全局暂停控制器
 *
 * 功能说明:
 *   当配置文件中 enable_pause_key=1 且 enable_display=1 时，
 *   在 GTK 显示窗口按下空格键即可暂停/恢复整个程序：
 *     - 所有通道的解码回调线程均阻塞于 wait_if_paused()
 *     - 推理队列无新输入，NPU 推理自然停止
 *     - 业务逻辑(channel_logic / global_logic)不再被调用
 *     - GTK 窗口继续刷新，但显示的是最后一帧（冻结画面）
 *     - 所有变量保持暂停瞬间的状态，不再更新
 *   再次按下空格键恢复运行。
 *
 * 接口:
 *   pause_ctrl::init(bool)      程序启动时根据配置调用一次
 *   pause_ctrl::toggle()        GTK 空格键回调中调用
 *   pause_ctrl::wait_if_paused() 工作线程检查点，功能关闭时零开销返回
 *   pause_ctrl::resume_all()    退出时调用，解除所有阻塞线程
 *   pause_ctrl::is_paused()     查询当前是否处于暂停状态
 *
 * 安全性:
 *   功能未启用 (g_enabled=false) 时 wait_if_paused() 立即返回，
 *   对原有逻辑没有任何影响。
 */
#pragma once
#include <atomic>

namespace pause_ctrl {

/** 是否启用暂停键功能 (程序启动时根据配置设置, 之后不再修改) */
extern std::atomic<bool> g_enabled;

/**
 * @brief 初始化暂停控制器
 * @param enabled true = 启用; false = 禁用 (所有接口立即返回, 零开销)
 * 仅在 main() 启动阶段调用一次。
 */
void init(bool enabled);

/**
 * @brief 切换暂停/运行状态
 * 由 GTK 键盘事件回调调用 (运行在 GTK 主线程)。
 * 功能未启用时立即返回。
 */
void toggle();

/**
 * @brief 工作线程暂停检查点
 *
 * 若当前处于暂停状态则阻塞，直到恢复运行或程序退出。
 * 若功能未启用 (g_enabled=false) 则立即返回，无任何锁操作。
 *
 * 调用位置: videoOutHandle() 入口处（解码回调线程）
 */
void wait_if_paused();

/**
 * @brief 强制解除所有阻塞线程
 * 程序退出前调用，防止工作线程永久阻塞导致无法退出。
 */
void resume_all();

/** 查询当前是否处于暂停状态 (功能未启用时始终返回 false) */
bool is_paused();

} // namespace pause_ctrl
