// Inject synthetic output tensors into real decoders, independent of image content.
// Usage: yolov5.rknn labels.txt yolov8_pose.rknn. Models initialize normal object lifetimes;
// tests themselves call only CPU postprocessing and never submit an inference.
#include "vision/inference/npu_device_owner.h"
#include "vision/models/yolo/yolo.h"
#include "vision/models/yolo/yolopose.h"
#include <iostream>
#include <limits>

struct DetectionThresholdTestAccess
{
    static int failures;
    static void check(bool ok, const char *name, float threshold)
    {
        if (!ok)
        {
            ++failures;
            std::cerr << "FAIL " << name << " threshold=" << threshold << '\n';
        }
    }

    static void detection(YOLO &model)
    {
        model.model_h_ = 8;
        model.num_classes_ = 1;
        model.io_num_out_ = 3;
        model.out_attrs_.resize(3);
        LetterBoxInfo lb;
        // Both objectness and class probability are .875, but their product is .765625.
        for (bool quantized : {false, true})
            for (bool anchor_last : {false, true})
            {
                model.is_quant_ = quantized;
                auto &attr = model.out_attrs_[0];
                attr = {};
                attr.n_dims = anchor_last ? 5 : 4;
                attr.dims[1] = anchor_last ? 3 : 18;
                attr.dims[4] = 6;
                attr.zp = 0;
                attr.scale = .125f;
                std::vector<float> floating(18, .5f);
                std::vector<int8_t> quantized_data(18, 4);
                for (int anchor = 0; anchor < 3; ++anchor)
                    for (int component : {4, 5})
                    {
                        floating[anchor * 6 + component] = .875f;
                        quantized_data[anchor * 6 + component] = 7;
                    }
                const float score = .765625f;
                for (float threshold : {0.f, .3f, score, std::nextafter(score, 1.f), .8f, .9f, 1.f})
                {
                    model.set_thresh(threshold, .45f);
                    std::vector<AlgoResult> results;
                    model.post_process(quantized ? static_cast<void *>(quantized_data.data())
                                                 : static_cast<void *>(floating.data()),
                                       0, 1, 1, lb, results);
                    check(results.size() == (score >= threshold ? 3u : 0u), "YOLOv5 product", threshold);
                    for (const auto &result : results)
                        check(result.score >= threshold, "YOLOv5 published score", threshold);
                }
            }
    }

    template <typename T> static void pose_quantized(YoloPose &model, int zp)
    {
        // Tests a rounded threshold near logit=1, and a threshold above the tensor maximum.
        for (int value : {0, 10, 127})
        {
            std::vector<T> input(65, static_cast<T>(zp));
            input[64] = static_cast<T>(value + zp);
            const float scale = value == 127 ? .01f : .1f;
            const float score = sigmoid(value * scale);
            for (float threshold : {0.f, .3f, score, std::nextafter(score, 1.f), .9f, 1.f})
            {
                model.set_thresh(threshold, .45f);
                std::vector<float> boxes, scores;
                std::vector<int> classes;
                const int count = model.process_quantized(input.data(), 1, 1, 8, boxes, scores, classes, zp, scale, 0);
                check(count == (score >= threshold ? 1 : 0), "Pose quantized boundary", threshold);
                for (float accepted : scores)
                    check(std::isfinite(accepted) && accepted >= threshold, "Pose published score", threshold);
            }
        }
    }

    static void pose(YoloPose &model)
    {
        pose_quantized<int8_t>(model, 0);
        pose_quantized<uint8_t>(model, 128);
        for (float value : {0.f, 1.f, -20.f, std::numeric_limits<float>::quiet_NaN()})
        {
            std::vector<float> input(65, 0.f);
            input[64] = value;
            const float score = sigmoid(value);
            for (float threshold : {0.f, .5f, .9f, 1.f})
            {
                model.set_thresh(threshold, .45f);
                std::vector<float> boxes, scores;
                std::vector<int> classes;
                const int count = model.process_fp32(input.data(), 1, 1, 8, boxes, scores, classes, 0, 1.f, 0);
                check(count == (std::isfinite(score) && score >= threshold ? 1 : 0), "Pose float boundary", threshold);
            }
        }
    }
};
int DetectionThresholdTestAccess::failures = 0;

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    npu::DeviceOwner owner;
    YOLO detection(argv[1], argv[2], 1);
    YoloPose pose(argv[3], "", 1);
    DetectionThresholdTestAccess::detection(detection);
    DetectionThresholdTestAccess::pose(pose);
    std::cout << "DECODER_FAILURES=" << DetectionThresholdTestAccess::failures << '\n';
    return DetectionThresholdTestAccess::failures ? 1 : 0;
}
