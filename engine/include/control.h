/** @file control.h
 * @brief 临时开启、关闭或查看通道的模型识别开关。
 * 在已启动的视频程序中使用；当前通道 ID 可直接取 ctx->chnId。
 */
#pragma once

/** @brief 临时开启或关闭某个通道的模型识别，例如手动暂停检测。
 * @param channel_id 配置中的通道 ID；当前通道传 ctx->chnId，不是画面的排列序号。
 * @param enable true 开启识别，false 关闭识别；不停止视频播放。
 * @return 没有返回值；通道不存在或视频程序未启动时不做操作。
 * 只修改本次运行的开关，不保存到配置文件；只在需要切换时调用，不必每张图重复设置。
 * 示例：
 * @code
 * logic_control_set_channel_inference(ctx->chnId, false); // 暂停当前通道
 * // 需要恢复时，另行调用并把第二个参数设为 true。
 * @endcode
 */
void logic_control_set_channel_inference(int channel_id, bool enable);

/** @brief 查看某个通道当前有没有打开模型识别开关。
 * @param channel_id 配置中的通道 ID；查询当前通道时传 ctx->chnId。
 * @return 1 表示开关已打开，0 表示已关闭；通道不存在或视频程序未启动时也返回 0。
 * 开关打开不保证模型成功运行，更不表示已经检测到目标；当前结果是否可用要看查询结果。
 * 示例：
 * @code
 * int enabled = logic_control_get_channel_inference(ctx->chnId);
 * draw_text(ctx, enabled ? "识别已开启" : "识别已关闭", {20, 40});
 * @endcode
 */
int logic_control_get_channel_inference(int channel_id);
