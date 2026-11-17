#pragma once

#include "model_group.h"
#include <config_types.h>
#include <memory>
#include <set>

std::set<int> names_to_class_ids(const std::vector<std::string> &names, const std::string &label_path);

// 调用者明确给出解码阈值：视频 ByteTrack 可降低它，图片使用模型配置值。
struct ModelBuildSpec
{
    ChannelModelConfig config;
    float decode_threshold;
};
std::shared_ptr<ModelGroup> create_model_group(const std::vector<ModelBuildSpec> &specs, int channel_id,
                                               int worker_index, int auto_core_pool);
