#include "inference/npu_device_owner.h"
#include "inference/inference_roi_geometry.h"
#include "inference/inference_result_mapping.h"
#include "yolo/yolo.h"
#include "yolo/yolov8det.h"
#include "yolo/yolopose.h"
#include "yolo/yoloseg.h"
#include "yolo/yolo26pose.h"
#include "yolo/yolo26depth.h"
#include "zip-depth/zip_depth.h"
#include <cassert>
#include <atomic>
#include <iostream>
#include <thread>

static std::unique_ptr<ModelBase> make_model(const std::string &type, const char *model, const char *labels)
{
    const int mask = 1; // all three contexts intentionally share core 0
    if (type == "yolov5") return std::unique_ptr<ModelBase>(new YOLO(model, labels, mask));
    if (type == "yolov8_obb") return std::unique_ptr<ModelBase>(new YoloV8Det(model, labels, mask, .5f, .4f, true));
    if (type == "yolov8_det") return std::unique_ptr<ModelBase>(new YoloV8Det(model, labels, mask));
    if (type == "yolov8_pose") return std::unique_ptr<ModelBase>(new YoloPose(model, labels, mask));
    if (type == "yolov5_seg") return std::unique_ptr<ModelBase>(new YoloSeg(model, labels, mask));
    if (type == "yolo26_pose") return std::unique_ptr<ModelBase>(new Yolo26Pose(model, labels, mask));
    if (type == "yolo26_depth") return std::unique_ptr<ModelBase>(new Yolo26Depth(model, mask));
    if (type == "zip_depth") return std::unique_ptr<ModelBase>(new ZipDepth(model, mask));
    throw std::runtime_error("unknown test model type");
}

int main(int argc, char **argv)
{
    assert(argc == 5); // type, model path, labels (may be empty), image
    npu::DeviceOwner owner;
    cv::setNumThreads(2);
    const cv::Mat image = cv::imread(argv[4]);
    assert(!image.empty());
    std::vector<std::unique_ptr<ModelBase>> models;
    for (int i = 0; i < 3; ++i) models.push_back(make_model(argv[1], argv[2], argv[3]));
    std::vector<std::thread> workers;
    std::atomic<int> ordinary_calls{0}, dma_calls{0};
    for (auto &model : models)
    {
        ModelBase *instance = model.get();
        workers.emplace_back([&, instance] {
            cv::Mat frame = image.clone();
            std::vector<AlgoResult> results;
            std::vector<AlgoResult> baseline;
            assert(instance->infer(frame, baseline));
            for (int pass = 0; pass < 8; ++pass)
            {
                assert(instance->infer(frame, results));
                assert(results.size() == baseline.size());
                for (size_t i=0;i<results.size();++i) {
                    assert(results[i].class_id == baseline[i].class_id);
                    assert(results[i].box == baseline[i].box);
                    assert(std::abs(results[i].score - baseline[i].score) < 1e-5);
                }
                ++ordinary_calls;
                // This exercises the DMA adapter entry, not a numerical parity
                // comparison: detectors use distinct CPU/zero-copy transforms.
                if (instance->get_input_fd() >= 0)
                {
                    assert(instance->infer_zero_copy(results));
                    ++dma_calls;
                }
            }
            for (const std::string mode : {"stretch", "letterbox", "expand"}) {
                const int w=instance->input_width(), h=instance->input_height();
                auto transform=make_inference_roi_transform({100,100,300,200},frame.cols,frame.rows,w,h,mode,true);
                cv::Mat roi=make_inference_roi_input(frame,transform,w,h);
                assert(roi.cols==w && roi.rows==h);
                assert(instance->infer(roi,results));
                map_results_to_business_frame(frame.cols,frame.rows,transform,w,h,results);
                for (const auto &r : results) {
                    if (!r.box.empty()) assert((r.box & cv::Rect(0,0,640,640)) == r.box);
                    if (!r.boxMask.empty()) assert(r.boxMask.size()==cv::Size(640,640));
                    if (r.is_depth()) assert(r.depth_map.size()==cv::Size(640,640));
                }
            }
        });
    }
    for (auto &worker : workers) worker.join();
    assert(ordinary_calls == 24);
    std::cout << argv[1] << ": three same-core contexts: ordinary_calls=" << ordinary_calls.load()
              << " dma_calls=" << dma_calls.load() << " PASS\n";
}
