#pragma once

#include "config/config.h"
#include <types.h>

#include <vector>

/*
 * 保留检测框中心落在归一化 polygon 内的结果。结果坐标位于
 * canonical_width × canonical_height 坐标系。
 *
 * 分割模型把整帧合并掩码挂在首个结果上；过滤时需要先保存该掩码、清空 ROI 外像素，
 * 再挂回首个保留结果，避免承载掩码的目标恰好位于 ROI 外时丢失区域内掩码。
 */
void filter_results_to_inference_roi(std::vector<AlgoResult> &results, const InferenceRoiConfig &roi,
                                     int canonical_width, int canonical_height);
