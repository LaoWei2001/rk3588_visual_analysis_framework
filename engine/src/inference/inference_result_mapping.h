#pragma once

#include "inference/inference_roi_geometry.h"
#include "inference/inference_types.h"

/* 在 tracker/logic 之前，将实际模型结果映射到完整视频的固定 640×640 业务坐标。
 * 不访问图像或创建业务帧；仅当结果含有掩码且需要变换时才处理掩码像素。 */
void map_results_to_business_frame(int source_width, int source_height, const InferenceRoiTransform &transform,
                                   int model_width, int model_height, std::vector<AlgoResult> &results);
