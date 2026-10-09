#include "model_status.h"

#include <map>
#include <mutex>
#include <sys/stat.h>
#include "common/business_coordinates.h"
#include "third_party/json/cJSON.h"
#include "yolo/composite_model.h"

namespace
{
struct ActiveModel
{
    std::string id, type, path, file_version;
    int width = 0, height = 0;
};
struct ChannelStatus
{
    std::string state = "disabled", error;
    std::vector<ActiveModel> active;
};
std::mutex status_mutex;
std::map<int, ChannelStatus> statuses;

std::string file_version(const std::string &path)
{
    struct stat info{};
    if (stat(path.c_str(), &info) != 0) return "";
    return std::to_string(info.st_ino) + ":" + std::to_string(info.st_size) + ":" +
           std::to_string(info.st_mtim.tv_sec) + ":" + std::to_string(info.st_mtim.tv_nsec);
}
}

void model_status_reset(const AppConfig &config)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    statuses.clear();
    for (const auto &channel : config.channels)
        statuses[channel.id] = ChannelStatus{};
}

void model_status_begin(int channel, const ChannelConfig &)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    statuses[channel].state = "loading";
    statuses[channel].error.clear();
}

void model_status_applied(int channel, const ChannelConfig &config,
                          const std::vector<std::shared_ptr<ModelBase>> &models)
{
    std::vector<ActiveModel> active;
    if (!models.empty() && models[0])
    {
        const auto composite = std::dynamic_pointer_cast<CompositeModel>(models[0]);
        size_t index = 0;
        for (const auto &spec : config.models)
        {
            if (!spec.enable || spec.model_path.empty() || spec.model_type.empty()) continue;
            const auto child = composite ? composite->entries().at(index).model : models[0];
            active.push_back({spec.id.empty() ? "model_" + std::to_string(index) : spec.id,
                              spec.model_type, spec.model_path, file_version(spec.model_path),
                              child->input_width(), child->input_height()});
            ++index;
        }
    }
    std::lock_guard<std::mutex> lock(status_mutex);
    auto &status = statuses[channel];
    status.active = std::move(active);
    status.state = status.active.empty() ? "disabled" : "applied";
    status.error.clear();
}

void model_status_failed(int channel, const std::string &error, bool clear_active)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    auto &status = statuses[channel];
    status.state = "failed";
    status.error = error;
    if (clear_active) status.active.clear();
}

void model_status_stopped()
{
    std::lock_guard<std::mutex> lock(status_mutex);
    for (auto &entry : statuses)
    {
        entry.second.state = "stopped";
        entry.second.active.clear();
    }
}

std::string inference_model_status_json()
{
    std::map<int, ChannelStatus> snapshot;
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        snapshot = statuses;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "business_width", business_coordinates::WIDTH);
    cJSON_AddNumberToObject(root, "business_height", business_coordinates::HEIGHT);
    cJSON *channels = cJSON_AddArrayToObject(root, "channels");
    for (const auto &entry : snapshot)
    {
        cJSON *channel = cJSON_CreateObject();
        cJSON_AddNumberToObject(channel, "channel_id", entry.first);
        cJSON_AddStringToObject(channel, "state", entry.second.state.c_str());
        cJSON_AddStringToObject(channel, "error", entry.second.error.c_str());
        cJSON *models = cJSON_AddArrayToObject(channel, "active_models");
        for (const auto &model : entry.second.active)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", model.id.c_str());
            cJSON_AddStringToObject(item, "model_type", model.type.c_str());
            cJSON_AddStringToObject(item, "model_path", model.path.c_str());
            cJSON_AddStringToObject(item, "file_version", model.file_version.c_str());
            cJSON_AddNumberToObject(item, "width", model.width);
            cJSON_AddNumberToObject(item, "height", model.height);
            cJSON_AddItemToArray(models, item);
        }
        cJSON_AddItemToArray(channels, channel);
    }
    char *json = cJSON_PrintUnformatted(root);
    std::string result = json ? json : "{\"ok\":false}";
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}
