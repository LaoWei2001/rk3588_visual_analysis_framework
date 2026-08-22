#pragma once
#include "config/config.h"
#include "inference/inference_types.h"
#include <memory>
#include <vector>

class LazyVideoFrame;

int inference_init(const AppConfig &cfg);
int inference_process_source(int chnId, void *source_data, int fd, int srcW, int srcH, int srcFmt, int srcStrH,
                             int srcStrV, int64_t frame_seq = 0, uint64_t frame_steady_ms = 0,
                             uint64_t frame_unix_ms = 0);
void inference_deinit();
/** 停止并唤醒推理/结果等待线程，但不 join、也不销毁同步对象。 */
void inference_request_stop();
/*
 * out_frame 是产出 out 这批检测结果时对应的惰性帧句柄；这里只传递稳定 DMA-BUF 引用，不转换像素。
 * out_frame_steady_ms/out_frame_unix_ms 是该帧进入分析管线时的双时钟。
 * logic 用这一组数据做"图像 + 检测框 + 时间"一致的报警/上报。
 * 没有新结果时返回 false, out 和 out_frame 都被 clear/release。
 */
bool inference_take_results(int chnId, std::vector<AlgoResult> &out, std::shared_ptr<LazyVideoFrame> &out_frame,
                            int64_t &out_frame_id, uint64_t &out_frame_steady_ms, uint64_t &out_frame_unix_ms);
int inference_get_input_w();
int inference_get_input_h();
float inference_get_infer_fps(int chnId);
void inference_update_thresh(int chnId, const ChannelConfig &config);
void inference_update_detect_classes(int chnId, const ChannelConfig &config);
void inference_update_queue_size(int queue_size);
/**
 * @return true 仅当新模型已经完整启用；false 表示旧模型仍保持运行，调用方不得
 *         把新模型字段发布到运行配置。线程拓扑变化目前明确要求重启。
 */
bool inference_reload_channel_model(int chnId, const ChannelConfig &new_cfg);

/**
 * @brief 阻塞直到指定通道有新的推理结果就绪, 或超时。
 * 不消费结果 (has_new 由 inference_take_results 消费)。
 * 用于层次二分发器线程: NPU 完成后立即唤醒而不等待下一帧解码回调。
 * @param chnId   通道号
 * @param timeout_ms 最长等待毫秒数
 * @return true 若结果就绪 (不代表 has_new 仍为 true, 取决于竞争), false 超时/关机
 */
bool inference_wait_result(int chnId, int timeout_ms);
