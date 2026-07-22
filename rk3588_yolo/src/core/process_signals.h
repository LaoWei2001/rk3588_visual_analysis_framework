#pragma once

#include <csignal>

/* 信号处理函数只写 sig_atomic_t；停止、唤醒和暂停操作在正常线程上下文执行。 */
extern volatile sig_atomic_t g_shutdown_signal;
extern volatile sig_atomic_t g_pause_toggle_signal;

