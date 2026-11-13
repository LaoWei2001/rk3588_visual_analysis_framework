#include "vision/inference/inference_result_mapping.h"
#include "vision/inference/npu_device_owner.h"
#include "vision/models/yolo/yolopose.h"
#include "vision/models/yolo/yolov8det.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <rga/im2d.h>
#include <thread>

static void equal(const std::vector<AlgoResult> &a, const std::vector<AlgoResult> &b)
{
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        assert(a[i].class_id == b[i].class_id && a[i].label == b[i].label);
        assert(std::abs(a[i].score - b[i].score) < 1e-5 && a[i].box == b[i].box);
        assert(a[i].has_obb == b[i].has_obb);
        if (a[i].has_obb)
            for (int k = 0; k < 4; ++k)
                assert(cv::norm(a[i].obb_corners[k] - b[i].obb_corners[k]) < .001);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    npu::DeviceOwner owner;
    cv::setNumThreads(1);
    std::string root = argv[1];
    cv::Mat image = cv::imread(root + "/yolov8_obb_demo/model/test.jpg");
    assert(!image.empty());
    for (bool separate : {false, true})
    {
        YoloV8Det obb(root + "/projects/assets/yolov8n_obb.rknn", root + "/projects/assets/labels_dota.txt", 1, .5, .4,
                      true);
        YoloV8Det obb_other(root + "/projects/assets/yolov8n_obb.rknn", "", separate ? 2 : 1, .5, .4, true);
        YoloV8Det det(root + "/projects/assets/yolov8.rknn", root + "/projects/assets/labels_helmet.txt",
                      separate ? 4 : 1);
        YoloPose pose(root + "/projects/assets/yolov8_pose.rknn", "", separate ? 4 : 1);
        std::vector<ModelBase *> models{&obb, &obb_other, &det, &pose};
        std::vector<std::vector<AlgoResult>> reference(4);
        for (int i = 0; i < 4; ++i)
            assert(models[i]->infer(image, reference[i]));
        assert(!reference[0].empty() && reference[0][0].has_obb);
        assert(reference[0][0].label != reference[1][0].label);
        std::vector<std::thread> threads;
        std::atomic<int> calls{0};
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&, i] {
                for (int n = 0; n < 30; ++n)
                {
                    std::vector<AlgoResult> results;
                    pthread_mutex_lock(&models[i]->infer_mtx);
                    assert(models[i]->infer(image, results));
                    equal(reference[i], results);
                    pthread_mutex_unlock(&models[i]->infer_mtx);
                    ++calls;
                }
            });
        for (auto &t : threads)
            t.join();
        assert(calls == 120);
        assert(obb.get_input_rga_handle() != 0);
        cv::Mat rgb;
        cv::cvtColor(image, rgb, cv::COLOR_BGR2RGB);
        auto src_handle = importbuffer_virtualaddr(rgb.data, rgb.total() * rgb.elemSize());
        assert(src_handle);
        auto src = wrapbuffer_handle(src_handle, 640, 640, RK_FORMAT_RGB_888);
        auto dst = wrapbuffer_handle(obb.get_input_rga_handle(), 640, 640, RK_FORMAT_RGB_888);
        im_opt_t opt{};
        opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
        assert(improcess(src, dst, {}, {0, 0, 640, 640}, {0, 0, 640, 640}, {}, 0, nullptr, &opt, 0) ==
               IM_STATUS_SUCCESS);
        releasebuffer_handle(src_handle);
        std::vector<AlgoResult> dma;
        ModelPerfStat stat;
        double infer = 0, post = 0;
        for (int n = 0; n < 20; ++n)
        {
            assert(obb.infer_zero_copy(dma, &stat));
            equal(reference[0], dma);
            infer += stat.infer_ms;
            post += stat.postprocess_ms;
        }
        std::cout << "OBB mixed concurrency separate=" << separate << " calls=" << calls
                  << " DMA parity=PASS infer_ms=" << infer / 20 << " post_ms=" << post / 20 << std::endl;
    }
    AlgoResult box;
    box.has_obb = true;
    box.box = {100, 100, 100, 100};
    box.obb_corners = {{{100, 100}, {200, 100}, {200, 200}, {100, 200}}};
    std::vector<AlgoResult> mapped{box};
    InferenceRoiTransform transform{{320, 0, 640, 720}, {0, 0, 640, 640}};
    map_results_to_business_frame(1280, 720, transform, 640, 640, mapped);
    assert(cv::norm(mapped[0].obb_corners[0] - cv::Point2f(210, 100)) < .001);
    assert(cv::norm(mapped[0].obb_corners[2] - cv::Point2f(260, 200)) < .001);
    std::cout << "OBB anisotropic ROI mapping PASS\n";
}
