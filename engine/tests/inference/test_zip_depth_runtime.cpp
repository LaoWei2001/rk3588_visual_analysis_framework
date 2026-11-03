#include "inference/npu_device_owner.h"
#include "model/zip-depth/zip_depth.h"
#include "model/zip-depth/depth_resize.h"
#include "inference/inference_result_mapping.h"
#include "pipeline/frame_transform.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>
#include <fcntl.h>
#include <unistd.h>

// Independent inputs_set reference follows the supplied standalone runner,
// exercising normalization/layout separately from our DMA input binding.
static cv::Mat reference(const char *path, const cv::Mat &image, cv::Size size, int core)
{
    rknn_context context = 0;
    assert(rknn_init(&context, const_cast<char *>(path), 0, 0, nullptr) == 0);
    assert(rknn_set_core_mask(context, static_cast<rknn_core_mask>(core)) == 0);
    cv::Mat rgb;
    cv::resize(image, rgb, size);
    cv::cvtColor(rgb, rgb, cv::COLOR_BGR2RGB);
    rknn_input input{};
    input.buf = rgb.data;
    input.size = rgb.total() * rgb.elemSize();
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    assert(rknn_inputs_set(context, 1, &input) == 0);
    assert(rknn_run(context, nullptr) == 0);
    rknn_output output{};
    output.want_float = 1;
    assert(rknn_outputs_get(context, 1, &output, nullptr) == 0);
    assert(output.size == size.area() * sizeof(float));
    cv::Mat result = cv::Mat(size, CV_32FC1, output.buf).clone();
    assert(rknn_outputs_release(context, 1, &output) == 0);
    assert(rknn_destroy(context) == 0);
    return result;
}

int main(int argc, char **argv)
{
    npu::DeviceOwner device_owner;
    assert(argc >= 2);
    cv::setNumThreads(2);
    const int core = argc > 3 ? std::stoi(argv[3]) : RKNN_NPU_CORE_AUTO;
    cv::Mat image;
    if (argc >= 3 && std::string(argv[2]) != "--synthetic") image = cv::imread(argv[2]);
    else
    {
        image.create(480, 864, CV_8UC3);
        cv::RNG rng(20261027);
        rng.fill(image, cv::RNG::UNIFORM, 0, 256);
    }
    assert(!image.empty());
    ZipDepth model(argv[1], core);
    const cv::Size size(model.input_width(), model.input_height());
    assert(model.get_input_fd() >= 0 && model.get_input_rga_handle() != 0);
    assert(!model.supports_roi_refinement());
    std::vector<AlgoResult> results;
    YoloPerfStat perf;
    assert(model.infer(image, results, &perf) && results.size() == 1);
    assert(results[0].is_relative_inverse_depth() && !results[0].is_metric_depth());
    cv::Mat original = results[0].depth_map.clone();
    cv::Mat expected = reference(argv[1], image, size, core);
    double error = cv::norm(original, expected, cv::NORM_INF);
    std::cout << "DMA vs standalone inputs_set max absolute error=" << error << '\n';
    assert(error < 1e-5);
    assert(model.infer_zero_copy(results));
    assert(cv::norm(original, results[0].depth_map, cv::NORM_INF) < 1e-5);

    // Actual RGA -> model DMA path, using known RGB input in a DMA allocation.
    // Same-size transfer isolates channel order/buffer layout from interpolation.
    const int heap = open("/dev/dma_heap/system-uncached", O_RDWR | O_CLOEXEC);
    assert(heap >= 0);
    dma_heap_allocation_data allocation{};
    allocation.len = size.area()*3;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    assert(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation) == 0);
    close(heap);
    void *memory = mmap(nullptr, allocation.len, PROT_READ|PROT_WRITE, MAP_SHARED, allocation.fd, 0);
    assert(memory != MAP_FAILED);
    cv::Mat bgr, rgb(size, CV_8UC3, memory);
    cv::resize(image, bgr, size);
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    auto source = rga_import_src_fd(allocation.fd, size.width, size.height, size.width, size.height, RK_FORMAT_RGB_888);
    assert(source);
    cv::Mat other(image.size(), CV_8UC3, cv::Scalar(0,0,0));
    assert(model.infer(other, results)); // require RGA to replace the previous DMA contents
    assert(rga_convert_resize_handle(0, *source, model.get_input_fd(), size.width, size.height,
                                    size.width, size.height, RK_FORMAT_RGB_888, model.get_input_rga_handle()));
    assert(model.infer_zero_copy(results));
    std::cout << "RGA depth error=" << cv::norm(expected, results[0].depth_map, cv::NORM_INF) << std::endl;
    assert(cv::norm(expected, results[0].depth_map, cv::NORM_INF) < 1e-5);
    source.reset();
    munmap(memory, allocation.len);
    close(allocation.fd);

    // All byte values and distinct RGB channels, beyond the adapter's startup
    // probe. Native FP16 must preserve the standalone RGB8 runner's output.
    cv::Mat ramp(size, CV_8UC3);
    for (int y = 0; y < ramp.rows; ++y)
        for (int x = 0; x < ramp.cols; ++x)
            ramp.at<cv::Vec3b>(y, x) = cv::Vec3b((x + y * 7) & 255, (x * 11 + y) & 255,
                                               (255 - x * 3 - y * 13) & 255);
    assert(model.infer(ramp, results));
    cv::Mat ramp_expected = reference(argv[1], ramp, size, core);
    const double ramp_error = cv::norm(ramp_expected, results[0].depth_map, cv::NORM_INF);
    std::cout << "RGB byte ramp max absolute error=" << ramp_error << '\n';
    assert(ramp_error == 0);

    std::vector<cv::Mat> retained, snapshots;
    for (int i = 0; i < 6; ++i)
    {
        cv::Mat frame(image.size(), CV_8UC3, cv::Scalar(i*30,80,140));
        assert(model.infer(frame, results));
        retained.push_back(results[0].depth_map);
        snapshots.push_back(results[0].depth_map.clone());
    }
    for (size_t i = 0; i < retained.size(); ++i)
        assert(cv::norm(retained[i], snapshots[i], cv::NORM_INF) == 0);
    cv::Mat padded;
    cv::copyMakeBorder(image, padded, 0, 0, 2, 2, cv::BORDER_CONSTANT);
    cv::Mat roi = padded(cv::Rect(2, 0, image.cols, image.rows));
    assert(!roi.isContinuous() && model.infer(roi, results));
    assert(cv::norm(original, results[0].depth_map, cv::NORM_INF) < 1e-5);
    cv::Mat empty, gray(10,10,CV_8UC1);
    assert(!model.infer(empty, results) && results.empty());
    assert(!model.infer(gray, results) && results.empty());

    for (int mode = 0; mode < 2; ++mode)
    {
        double total = 0, pre = 0, inference = 0, post = 0, mapping = 0;
        for (int i = 0; i < 12; ++i)
        {
            auto start = std::chrono::steady_clock::now();
            assert(mode ? model.infer_zero_copy(results, &perf) : model.infer(image, results, &perf));
            auto map_start = std::chrono::steady_clock::now();
            map_results_to_business_frame(image.cols, image.rows,
                {cv::Rect(0,0,image.cols,image.rows), cv::Rect(0,0,size.width,size.height)},
                size.width, size.height, results);
            assert(results[0].depth_map.size() == cv::Size(640,640));
            auto end = std::chrono::steady_clock::now();
            total += std::chrono::duration<double,std::milli>(end-start).count();
            mapping += std::chrono::duration<double,std::milli>(end-map_start).count();
            pre += perf.preprocess_ms; inference += perf.infer_ms; post += perf.postprocess_ms;
        }
        std::cout << (mode ? "DMA ready" : "BGR input") << " core=" << core << " mean_ms=" << total/12
                  << " preprocess=" << pre/12 << " NPU+get=" << inference/12 << " post=" << post/12
                  << " business_mapping=" << mapping/12 << '\n';
    }
    std::cout << "ZipDepth hardware parity, DMA/RGA, retained output, strided input: PASS\n";
}
