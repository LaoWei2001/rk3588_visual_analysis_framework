// Save deterministic detections for a before/after comparison on real hardware.
// Usage: model.rknn labels.txt image.png output.txt [model_type]
#include "vision/inference/npu_device_owner.h"
#include "vision/models/yolo/yolo.h"
#include "vision/models/yolo/yolo26depth.h"
#include "vision/models/yolo/yolopose.h"
#include "vision/models/yolo/yoloseg.h"
#include "yolov8det.h"
#include <cassert>
#include <fstream>
#include <iomanip>
#include <memory>
#include <rga/im2d.h>

int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    npu::DeviceOwner owner;
    cv::setNumThreads(1);
    const std::string type = argc == 6 ? argv[5] : "yolov8_det";
    auto create = [&]() -> std::unique_ptr<ModelBase> {
        if (type == "yolov5")
            return std::unique_ptr<ModelBase>(new YOLO(argv[1], argv[2], 1, .3f, .45f));
        if (type == "yolov8_pose")
            return std::unique_ptr<ModelBase>(new YoloPose(argv[1], argv[2], 1, .3f, .45f));
        if (type == "yolov5_seg")
            return std::unique_ptr<ModelBase>(new YoloSeg(argv[1], argv[2], 1, .3f, .45f));
        if (type == "yolo26_depth")
            return std::unique_ptr<ModelBase>(new Yolo26Depth(argv[1], 1));
        return std::unique_ptr<ModelBase>(new YoloV8Det(argv[1], argv[2], 1, .3f, .45f, type == "yolov8_obb"));
    };
    auto model = create();
    cv::Mat camera = cv::imread(argv[3]);
    assert(!camera.empty());
    const int w = model->input_width(), h = model->input_height();
    cv::Mat square;
    cv::resize(camera, square, {w, h});
    cv::Mat gradient(h, w, CV_8UC3);
    cv::RNG random(12345);
    random.fill(gradient, cv::RNG::UNIFORM, 0, 256);
    cv::Mat padded(h + 16, w + 16, CV_8UC3, cv::Scalar(114, 114, 114));
    square.copyTo(padded(cv::Rect(8, 8, w, h)));
    std::vector<cv::Mat> inputs{camera,
                                square,
                                gradient,
                                cv::Mat(h, w, CV_8UC3, cv::Scalar(0, 0, 0)),
                                cv::Mat(h, w, CV_8UC3, cv::Scalar(255, 255, 255)),
                                padded(cv::Rect(8, 8, w, h))};
    std::ofstream snapshot(argv[4]);
    assert(snapshot);
    snapshot << std::setprecision(9);
    // Separate contexts: legacy inputs_set invalidates previously bound DMA IO.
    model.reset();
    for (bool dma : {false, true})
    {
        model = create();
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            std::vector<AlgoResult> results;
            if (dma)
            {
                assert(model->get_input_rga_handle());
                cv::Mat resized, rgb;
                cv::resize(inputs[i], resized, {w, h});
                cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
                auto handle = importbuffer_virtualaddr(rgb.data, rgb.total() * rgb.elemSize());
                assert(handle);
                auto src = wrapbuffer_handle(handle, w, h, RK_FORMAT_RGB_888);
                auto dst = wrapbuffer_handle(model->get_input_rga_handle(), w, h, RK_FORMAT_RGB_888);
                assert(imcopy(src, dst) == IM_STATUS_SUCCESS);
                releasebuffer_handle(handle);
                assert(model->infer_zero_copy(results));
            }
            else
                assert(model->infer(inputs[i], results));
            snapshot << i << ' ' << dma << ' ' << results.size() << '\n';
            for (const auto &r : results)
            {
                snapshot << r.class_id << ' ' << r.score << ' ' << r.box.x << ' ' << r.box.y << ' ' << r.box.width
                         << ' ' << r.box.height << ' ' << r.label;
                if (r.has_obb)
                    for (const auto &p : r.obb_corners)
                        snapshot << ' ' << p.x << ' ' << p.y;
                for (size_t k = 0; k < r.keypoints.size(); ++k)
                    snapshot << ' ' << r.keypoints[k].x << ' ' << r.keypoints[k].y << ' ' << r.keypoint_scores[k];
                for (const auto &mat : {r.boxMask, r.depth_map})
                    if (!mat.empty())
                    {
                        uint64_t hash = 14695981039346656037ull;
                        for (int row = 0; row < mat.rows; ++row)
                            for (size_t b = 0; b < mat.cols * mat.elemSize(); ++b)
                                hash = (hash ^ mat.ptr<uint8_t>(row)[b]) * 1099511628211ull;
                        snapshot << ' ' << mat.rows << ' ' << mat.cols << ' ' << hash;
                    }
                snapshot << '\n';
            }
        }
    }
    assert(snapshot.good());
}
