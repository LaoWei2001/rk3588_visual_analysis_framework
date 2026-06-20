/**
 * @file constants.h
 * @brief 系统常量定义
 */
#pragma once

namespace constants {

// 通道相关
constexpr int MAX_CHANNELS = 15;

// 配置热加载
constexpr int CONFIG_MONITOR_INTERVAL_SEC = 2;

// 重连策略
constexpr int RECONNECT_MAX_BACKOFF_SEC = 32;
constexpr int RECONNECT_INITIAL_BACKOFF_SEC = 1;

// 结果新鲜度
constexpr int RESULT_MAX_AGE_MS = 100;

// 报警队列
constexpr int ALARM_QUEUE_SIZE = 64;

} // namespace constants
