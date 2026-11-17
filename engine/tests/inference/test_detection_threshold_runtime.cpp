// RK3588 regression: no decoder may return an object below its configured score.
// Usage: model_type model.rknn labels.txt video [frame_index]
// Exercises both CPU-prepared and DMA inputs, plus threshold changes on one instance.
#include "vision/inference/npu_device_owner.h"
#include "vision/models/yolo/yolo.h"
#include "vision/models/yolo/yolo26pose.h"
#include "vision/models/yolo/yolopose.h"
#include "vision/models/yolo/yoloseg.h"
#include "vision/models/yolo/yolov8det.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <rga/im2d.h>
#include <stdexcept>

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6)
        return 2;
    try
    {
        npu::DeviceOwner owner;
        cv::setNumThreads(1);
        const std::string type = argv[1];
        auto create = [&]() -> std::unique_ptr<ModelBase> {
            if (type == "yolov5")
                return std::unique_ptr<ModelBase>(new YOLO(argv[2], argv[3], 1));
            if (type == "yolov8_pose")
                return std::unique_ptr<ModelBase>(new YoloPose(argv[2], argv[3], 1));
            if (type == "yolo26_pose")
                return std::unique_ptr<ModelBase>(new Yolo26Pose(argv[2], argv[3], 1));
            if (type == "yolov5_seg")
                return std::unique_ptr<ModelBase>(new YoloSeg(argv[2], argv[3], 1));
            if (type == "yolov8_det" || type == "yolov8_obb")
                return std::unique_ptr<ModelBase>(new YoloV8Det(argv[2], argv[3], 1, .3f, .45f, type == "yolov8_obb"));
            throw std::runtime_error("Unsupported detection model type");
        };
        cv::VideoCapture video(argv[4], cv::CAP_FFMPEG);
        if (argc == 6)
            video.set(cv::CAP_PROP_POS_FRAMES, std::stoi(argv[5]));
        cv::Mat source;
        if (!video.read(source))
            throw std::runtime_error("Cannot read test video frame");
        int failures = 0, checked = 0;
        for (bool dma : {false, true})
        {
            auto model = create();
            cv::Mat frame, rgb;
            cv::resize(source, frame, {model->input_width(), model->input_height()});
            if (dma)
            {
                cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
                auto handle = importbuffer_virtualaddr(rgb.data, rgb.total() * rgb.elemSize());
                if (!handle || !model->get_input_rga_handle())
                    throw std::runtime_error("DMA input unavailable");
                auto src = wrapbuffer_handle(handle, rgb.cols, rgb.rows, RK_FORMAT_RGB_888);
                auto dst = wrapbuffer_handle(model->get_input_rga_handle(), rgb.cols, rgb.rows, RK_FORMAT_RGB_888);
                const auto status = imcopy(src, dst);
                releasebuffer_handle(handle);
                if (status != IM_STATUS_SUCCESS)
                    throw std::runtime_error("DMA copy failed");
            }
            // Includes 0/1, ordinary and high thresholds, and lowering after 1.
            for (float threshold : {.3f, .5f, .9f, .99f, 1.f, 0.f, .3f})
            {
                model->set_thresh(threshold, .45f);
                std::vector<AlgoResult> results;
                if (!(dma ? model->infer_zero_copy(results) : model->infer(frame, results)))
                    throw std::runtime_error("Inference failed");
                int below = 0;
                float minimum = 1.f;
                for (const auto &result : results)
                {
                    if (result.is_depth())
                        continue;
                    ++checked;
                    minimum = std::min(minimum, result.score);
                    if (!std::isfinite(result.score) || result.score < threshold)
                        ++below;
                }
                failures += below;
                std::cout << "THRESHOLD type=" << type << " dma=" << dma << " threshold=" << threshold
                          << " count=" << results.size() << " min_score=" << minimum << " below=" << below << '\n';
            }
        }
        std::cout << "CHECKED=" << checked << " BELOW_THRESHOLD_RESULTS=" << failures << '\n';
        return failures ? 1 : 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
