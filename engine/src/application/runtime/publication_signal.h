#pragma once

#include <cstdint>

/*
 * 通道对外发布新快照时递增序号并唤醒等待者。全局 Logic 用它实现“数据到达即运行”，
 * 同时保留配置的 poll_interval_ms 作为无新数据时的周期性兜底。
 */
uint64_t publication_signal_sequence(void);
void publication_signal_notify(void);
bool publication_signal_wait(uint64_t observed_sequence, int timeout_ms);
