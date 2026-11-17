#include "vision/inference/frame_executor.h"
#include "vision/models/model_group.h"
#include "vision/models/model_registry.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <dlfcn.h>
#include <inference.h>
#include <limits>
#include <pthread.h>
#include <stdexcept>

// Interpose only in this test binary: fail after one child thread was created.
static std::atomic<int> thread_creations_before_failure{-1};
extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *),
                              void *argument) noexcept
{
    using Create = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static const auto real_create = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    assert(real_create);
    const int remaining = thread_creations_before_failure.load();
    if (remaining >= 0 && thread_creations_before_failure.fetch_sub(1) == 0)
        return EAGAIN;
    return real_create(thread, attributes, function, argument);
}

namespace
{
class TestModel : public ModelBase
{
  public:
    bool fail = false;
    bool empty = false;
    int object_count = 3;
    TestModel(int width, int height, float threshold) : width_(width), height_(height), threshold_(threshold)
    {
    }
    int input_width() const override
    {
        return width_;
    }
    int input_height() const override
    {
        return height_;
    }
    void set_thresh(float threshold, float) override
    {
        threshold_ = threshold;
    }
    float get_obj_thresh() const override
    {
        return threshold_;
    }
    float get_nms_thresh() const override
    {
        return .45f;
    }
    bool nms_done() const override
    {
        return true;
    }
    bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, ModelPerfStat *perf) override
    {
        assert(frame.cols == width_ && frame.rows == height_);
        assert(frame.type() == CV_8UC3);
        if (fail)
            throw std::runtime_error("test model failure");
        results.clear();
        if (!empty)
        {
            for (int i = 0; i < object_count; ++i)
            {
                AlgoResult result;
                result.box = cv::Rect2f(width_ * (.1f + .3f * i), height_ * .25f, width_ * .1f, height_ * .25f);
                result.label = "person";
                result.class_id = 0;
                result.score = i == 0 ? .55f : i == 2 ? std::numeric_limits<float>::quiet_NaN() : .95f;
                results.push_back(result);
            }
        }
        if (perf)
            perf->infer_ms = 1;
        return true;
    }

  private:
    int width_, height_;
    float threshold_;
};

class DepthModel : public TestModel
{
  public:
    DepthModel() : TestModel(640, 640, 0.0f)
    {
    }
    bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, ModelPerfStat *) override
    {
        assert(frame.cols == 640 && frame.rows == 640);
        AlgoResult depth;
        depth.score = 1;
        depth.depth_map = cv::Mat(640, 640, CV_32FC1, cv::Scalar(2));
        depth.box = cv::Rect(0, 0, 640, 640);
        results = {depth};
        return true;
    }
};

InferenceTask task(const cv::Mat &source)
{
    InferenceTask value{};
    value.chnId = 0;
    value.srcW = source.cols;
    value.srcH = source.rows;
    value.srcStrH = source.cols;
    value.srcStrV = source.rows;
    value.srcFmt = RK_FORMAT_BGR_888;
    value.frame = std::make_shared<LazyVideoFrame>(0, nullptr, source.cols, source.rows, source.cols, source.rows,
                                                   RK_FORMAT_BGR_888, 640, 640, source.data);
    return value;
}
} // namespace

int main()
{
    auto low = std::make_shared<TestModel>(320, 480, .2f);
    auto high = std::make_shared<TestModel>(960, 544, .9f);
    low->set_result_source("low", "yolov8_det", 0);
    high->set_result_source("high", "yolov8_det", 1);
    ModelGroup::Entry a, b;
    a.id = "low";
    a.type = "yolov8_det";
    a.model = low;
    a.obj_thresh = .2f;
    a.nms_thresh = .45f;
    b.id = "high";
    b.type = "yolov8_det";
    b.model = high;
    b.obj_thresh = .9f;
    b.nms_thresh = .45f;
    auto group = std::make_shared<ModelGroup>(std::vector<ModelGroup::Entry>{a, b});
    FrameExecutor executor(0, group);
    assert(executor.model_count() == 2 && executor.is_parallel());
    cv::Mat pixels(720, 1280, CV_8UC3, cv::Scalar(20, 30, 40));
    auto input = task(pixels);
    std::vector<AlgoResult> results;
    std::vector<vision::ModelInferenceStatus> statuses;
    ModelPerfStat perf;
    float wait = 0;
    assert(executor.run(input, results, perf, wait, &statuses));
    filter_inference_results(results, *group, 0.0f, 1.0f, {});
    assert(results.size() == 3);
    assert(statuses.size() == 2 && statuses[0].succeeded && statuses[1].succeeded);
    assert(statuses[0].input_width == 320 && statuses[1].input_width == 960);
    int low_count = 0, high_count = 0;
    for (const auto &result : results)
    {
        assert(std::isfinite(result.score));
        assert(std::abs(result.box.y - 160) < .01f && std::abs(result.box.width - 64) < .01f);
        if (result.model_id == "low")
            ++low_count;
        else
        {
            assert(result.model_id == "high" && result.score >= .9f);
            ++high_count;
        }
    }
    assert(low_count == 2 && high_count == 1);
    vision::FrameInferenceResult output;
    output.results = results;
    output.models = statuses;
    assert(output.complete() && output.succeeded());
    auto painted = pixels.clone();
    vision::draw_inference_results(painted, output, "high");
    assert(cv::norm(painted, pixels, cv::NORM_L1) > 0);

    // Reuse workers after an exception: keep the other model's results and identity.
    high->fail = true;
    assert(executor.run(input, results, perf, wait, &statuses));
    filter_inference_results(results, *group, 0.0f, 1.0f, {});
    assert(results.size() == 2 && statuses[0].succeeded && !statuses[1].succeeded);
    assert(statuses[1].error == "test model failure");
    for (const auto &result : results)
        assert(result.model_id == "low");
    low->empty = true;
    assert(executor.run(input, results, perf, wait, &statuses));
    assert(results.empty() && statuses[0].succeeded && !statuses[1].succeeded);
    high->fail = false;
    high->empty = true;
    assert(executor.run(input, results, perf, wait, &statuses));
    assert(results.empty() && statuses[0].succeeded && statuses[1].succeeded);
    high->empty = false;
    assert(executor.run(input, results, perf, wait, &statuses));
    filter_inference_results(results, *group, 0.0f, 1.0f, {});
    assert(results.size() == 1 && results.front().model_id == "high");

    // The same execution path must remain correct after partial thread startup fails.
    low->empty = false;
    thread_creations_before_failure.store(1);
    {
        FrameExecutor fallback(0, group);
        assert(!fallback.is_parallel() && fallback.model_count() == 2);
        assert(thread_creations_before_failure.load() == -1);
        assert(fallback.run(input, results, perf, wait, &statuses));
        filter_inference_results(results, *group, 0.0f, 1.0f, {});
        assert(results.size() == 3 && statuses[0].input_width == 320 && statuses[1].input_width == 960);
        high->fail = true;
        assert(fallback.run(input, results, perf, wait, &statuses));
        assert(statuses[0].succeeded && !statuses[1].succeeded);
        assert(pthread_mutex_trylock(&high->infer_mtx) == 0);
        pthread_mutex_unlock(&high->infer_mtx);
        high->fail = false;
    }
    // Single models also use the group executor and retain the 100-result filter path.
    {
        auto single = std::make_shared<ModelGroup>(std::vector<ModelGroup::Entry>{a});
        FrameExecutor one(0, single);
        assert(one.model_count() == 1 && !one.is_parallel());
        assert(one.run(input, results, perf, wait, &statuses));
        filter_inference_results(results, *single, .9f, .45f, {});
        assert(results.size() == 1 && results[0].score >= .9f);
        low->object_count = 120;
        assert(one.run(input, results, perf, wait, &statuses));
        filter_inference_results(results, *single, 0.0f, .45f, {});
        assert(results.size() == 100); // A one-model group must not inherit the multi-model 20-object cap.
        low->object_count = 3;
        low->fail = true;
        assert(!one.run(input, results, perf, wait, &statuses));
        assert(!statuses[0].succeeded && results.empty());
        assert(pthread_mutex_trylock(&low->infer_mtx) == 0);
        pthread_mutex_unlock(&low->infer_mtx);
        low->fail = false;
    }
    // Mixed dimensions and depth survive model ordering and class filtering.
    low->empty = true;
    ModelGroup::Entry depth;
    depth.id = "depth";
    depth.model = std::make_shared<DepthModel>();
    depth.allowed_classes = {123};
    for (const char *type : {"yolo26_depth", "zip_depth"})
        for (bool depth_first : {false, true})
        {
            depth.type = type;
            auto mixed = std::make_shared<ModelGroup>(depth_first ? std::vector<ModelGroup::Entry>{depth, a}
                                                                  : std::vector<ModelGroup::Entry>{a, depth});
            FrameExecutor mixed_executor(0, mixed);
            assert(mixed_executor.run(input, results, perf, wait, &statuses));
            assert(results.size() == 1 && results[0].is_depth());
            assert(results[0].depth_map.size() == cv::Size(640, 640));
            assert(results[0].depth_at(320, 320) == 2);
            assert(results[0].model_id == "depth" && results[0].model_type == type);
        }

    // The common builder rejects missing groups and unsupported types before any RKNN calls.
    ChannelModelConfig unknown;
    unknown.enable = true;
    unknown.model_type = "unknown-test-model";
    unknown.model_path = "/not-opened.rknn";
    for (const auto &specs : {std::vector<ModelBuildSpec>{}, std::vector<ModelBuildSpec>{{unknown, .25f}}})
    {
        bool rejected_group = false;
        try
        {
            create_model_group(specs, 0, 0, 7);
        }
        catch (const std::invalid_argument &)
        {
            rejected_group = true;
        }
        assert(rejected_group);
    }

    // Invalid options fail before obtaining the NPU lease or constructing RKNN.
    bool rejected = false;
    try
    {
        vision::ImageInferenceSession session(vision::ImageInferenceOptions{});
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    assert(rejected);
    return 0;
}
