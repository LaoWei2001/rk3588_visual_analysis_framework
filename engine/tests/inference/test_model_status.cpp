#include <cassert>
#include <iostream>
#include <thread>
#include "inference/model_status.h"
#include "third_party/json/cJSON.h"
#include "yolo/composite_model.h"

class FakeModel : public ModelBase
{
    int dimension_;
public:
    explicit FakeModel(int dimension) : dimension_(dimension) {}
    bool infer(cv::Mat &, std::vector<AlgoResult> &, YoloPerfStat *) override { return true; }
    int input_width() const override { return dimension_; }
    int input_height() const override { return dimension_; }
    void set_thresh(float, float) override {}
    float get_obj_thresh() const override { return .3f; }
    float get_nms_thresh() const override { return .45f; }
};

void check(const char *state, int width, int active_count = 1)
{
    cJSON *root = cJSON_Parse(inference_model_status_json().c_str());
    assert(root);
    assert(cJSON_GetObjectItem(root, "business_width")->valueint == 640);
    const auto channel = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "channels"), 0);
    assert(std::string(cJSON_GetObjectItem(channel, "state")->valuestring) == state);
    const auto models = cJSON_GetObjectItem(channel, "active_models");
    assert(cJSON_GetArraySize(models) == active_count);
    for (int index = 0; index < active_count; ++index)
        assert(cJSON_GetObjectItem(cJSON_GetArrayItem(models, index), "width")->valueint == width);
    cJSON_Delete(root);
}

int main()
{
    AppConfig config;
    ChannelConfig channel;
    channel.id = 2;
    ChannelModelConfig spec;
    spec.id = "det";
    spec.model_path = "assets/small.rknn";
    spec.model_type = "yolov8_det";
    channel.models = {spec};
    config.channels = {channel};
    model_status_reset(config);
    check("disabled", 0, 0);
    model_status_begin(2, channel);
    model_status_applied(2, channel, {std::make_shared<FakeModel>(640)});
    check("applied", 640);
    channel.models[0].model_path = "assets/large.rknn";
    model_status_begin(2, channel);
    check("loading", 640);
    model_status_failed(2, "load failed");
    check("failed", 640);
    assert(inference_model_status_json().find("assets/small.rknn") != std::string::npos);
    model_status_begin(2, channel);
    model_status_applied(2, channel, {std::make_shared<FakeModel>(960)});
    check("applied", 960);
    // Polling and lifecycle writes can proceed concurrently with no inference lock.
    std::thread reader([] { for (int i = 0; i < 500; ++i) check("applied", 960); });
    for (int i = 0; i < 500; ++i)
        model_status_applied(2, channel, {std::make_shared<FakeModel>(960)});
    reader.join();
    channel.models.push_back(spec);
    CompositeModel::Entry first, second;
    first.id = "a"; first.model = std::make_shared<FakeModel>(960);
    second.id = "b"; second.model = std::make_shared<FakeModel>(960);
    model_status_applied(2, channel, {std::make_shared<CompositeModel>(std::vector<CompositeModel::Entry>{first, second})});
    check("applied", 960, 2);
    model_status_applied(2, channel, {});
    check("disabled", 0, 0);
    model_status_stopped();
    check("stopped", 0, 0);
    model_status_reset(config);
    check("disabled", 0, 0);
    std::cout << "Model lifecycle snapshot regression passed\n";
}
