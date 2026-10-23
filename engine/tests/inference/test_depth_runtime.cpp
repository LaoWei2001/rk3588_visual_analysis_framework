#include "yolo/yolo26depth.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>

int main(int argc, char **argv)
{
    assert(argc >= 3);
    cv::setNumThreads(2);
    cv::Mat image = cv::imread(argv[2]);
    assert(!image.empty());
    Yolo26Depth model(argv[1]);
    std::vector<AlgoResult> results;
    YoloPerfStat perf;
    assert(model.infer(image, results, &perf) && results.size() == 1);
    assert(results[0].depth_map.type() == CV_32FC1);
    cv::Mat full;
    cv::resize(results[0].depth_map, full, image.size());
    if (argc >= 4)
    {
        std::ifstream input(argv[3], std::ios::binary);
        assert(input.good());
        cv::Mat reference(image.size(), CV_32FC1);
        input.read(reinterpret_cast<char *>(reference.data), reference.total() * reference.elemSize());
        assert(input.gcount() == static_cast<std::streamsize>(reference.total() * reference.elemSize()));
        const double error = cv::norm(full, reference, cv::NORM_INF);
        std::cout << "Native vs upstream FP32 reference max absolute error: " << error << " m\n";
        assert(error < 1e-3);
    }
    assert(model.get_input_fd() >= 0); // hardware deployment must enable the DMA path
    const cv::Mat before = results[0].depth_map.clone();
    assert(model.infer_zero_copy(results, &perf)); // reuse the exact RGB DMA input
    assert(cv::norm(before, results[0].depth_map, cv::NORM_INF) < 1e-3);
    std::vector<cv::Mat> pinned, snapshots;
    for (int i = 0; i < 8; ++i)
    {
        cv::Mat frame(image.size(), CV_8UC3, cv::Scalar(i*20,80,140));
        assert(model.infer(frame, results));
        pinned.push_back(results[0].depth_map);
        snapshots.push_back(results[0].depth_map.clone());
    }
    for (size_t i = 0; i < pinned.size(); ++i)
        assert(cv::norm(pinned[i], snapshots[i], cv::NORM_INF) == 0);
    cv::Mat padded;
    cv::copyMakeBorder(image, padded, 0,0,2,2,cv::BORDER_CONSTANT);
    cv::Mat roi = padded(cv::Rect(2,0,image.cols,image.rows));
    assert(!roi.isContinuous() && model.infer(roi, results));
    assert(cv::norm(before, results[0].depth_map, cv::NORM_INF) < 1e-3);
    cv::Mat invalid;
    assert(!model.infer(invalid, results) && results.empty());
    for (int i = 0; i < 3; ++i) model.infer(image, results);
    for (int mode = 0; mode < 2; ++mode)
    {
        double total = 0, pre = 0, npu = 0, post = 0;
        for (int i = 0; i < 15; ++i)
        {
            auto start = std::chrono::steady_clock::now();
            assert(mode ? model.infer_zero_copy(results,&perf) : model.infer(image,results,&perf));
            total += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            pre += perf.preprocess_ms; npu += perf.infer_ms; post += perf.postprocess_ms;
        }
        std::cout << (mode ? "DMA-ready input" : "BGR CPU fallback") << ": avg=" << total/15
                  << " ms pre=" << pre/15 << " NPU+get=" << npu/15 << " post=" << post/15 << '\n';
    }
    std::cout << "Native NPU, DMA input, retained outputs, strided ROI and invalid frame: PASS\n";
}
