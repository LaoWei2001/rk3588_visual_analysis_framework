#pragma once
#include <channel.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
struct AppRuntimeSnapshot;

// An optional observer of the same inference frame, independent of channel Logic.
bool remote_dataset_active(int channel);
void remote_dataset_observe(ChannelContext &ctx, const std::shared_ptr<const AppRuntimeSnapshot> &runtime);
int remote_dataset_init(const std::string &socket_path);
void remote_dataset_deinit();
// JSON header followed by optional JPEG bytes on the local-only Unix socket.
std::string remote_dataset_command(const std::string &request, std::vector<unsigned char> &jpeg);

// Manual capture consumes the next decoded source before inference. The getter owns its source.
bool remote_dataset_manual_active(int channel);
void remote_dataset_manual_frame(int channel, int64_t frame, uint64_t timestamp, size_t bytes,
                                 std::function<cv::Mat()> acquire);
