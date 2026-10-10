/** @file control.h
 * @brief 通用运行控制：按通道 ID 查询或切换推理状态。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

/** 设置指定通道的运行时推理开关（不修改持久配置）。
 * true：下一帧恢复推理；false：跳过推理，画面继续显示。
 * 线程安全，可在通道或全局业务回调中调用。 */
void logic_control_set_channel_inference(int channel_id, bool enable);

/** 读取指定通道的运行时推理开关状态。 */
int logic_control_get_channel_inference(int channel_id);
