#pragma once

#include "application/config/config.h"
#include "vision/models/model_group.h"
#include <memory>
#include <string>
#include <vector>

// Lifecycle-only updates. Readers never take the inference dispatch lock.
void model_status_reset(const AppConfig &config);
void model_status_begin(int channel);
void model_status_applied(int channel, const ChannelConfig &config,
                          const std::vector<std::shared_ptr<ModelGroup>> &models);
void model_status_failed(int channel, const std::string &error, bool clear_active = false);
void model_status_stopped();
std::string inference_model_status_json();
