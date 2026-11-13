// Per-layer CPU/NPU diagnosis using the pinned RKNN SDK. Profiling changes
// execution time: use npu_bench / full video A/B runs for throughput claims.
#include <rknn_api.h>
#include "vision/inference/npu_device_owner.h"
#include "vision/inference/npu_execution.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void check(int rc, const char *operation)
{
    if (rc < 0) throw std::runtime_error(std::string(operation) + ": " + std::to_string(rc));
}
struct Model
{
    rknn_context context = 0;
    rknn_tensor_mem *input = nullptr;
    ~Model()
    {
        if (input) rknn_destroy_mem(context, input);
        if (context) rknn_destroy(context);
    }
};
double milliseconds(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "Usage: %s model.rknn [iterations=5]\n", argv[0]);
        return 1;
    }
    try
    {
        npu::DeviceOwner owner;
        Model model;
        const int iterations = argc == 3 ? std::stoi(argv[2]) : 5;
        if (iterations < 1) throw std::runtime_error("iterations must be positive");
        check(rknn_init(&model.context, argv[1], 0, RKNN_FLAG_COLLECT_PERF_MASK, nullptr), "init");
        check(rknn_set_core_mask(model.context, RKNN_NPU_CORE_0), "core 0");
        rknn_input_output_num io{};
        check(rknn_query(model.context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "query IO");
        if (io.n_input != 1 || !io.n_output) throw std::runtime_error("requires one image input and nonempty outputs");
        rknn_tensor_attr input{};
        check(rknn_query(model.context, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input)), "query input");
        if (input.n_dims != 4 || input.dims[0] != 1 ||
            (input.fmt != RKNN_TENSOR_NHWC && input.fmt != RKNN_TENSOR_NCHW))
            throw std::runtime_error("requires static batch-1 RGB input");
        const uint32_t width = input.dims[input.fmt == RKNN_TENSOR_NHWC ? 2 : 3];
        const uint32_t height = input.dims[input.fmt == RKNN_TENSOR_NHWC ? 1 : 2];
        const uint32_t channels = input.dims[input.fmt == RKNN_TENSOR_NHWC ? 3 : 1];
        if (channels != 3 || !width || !height || uint64_t(width) * height * 3 != input.n_elems)
            throw std::runtime_error("invalid RGB shape");
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        input.w_stride = width;
        input.h_stride = height;
        model.input = rknn_create_mem(model.context, std::max(input.n_elems, input.size_with_stride));
        if (!model.input || !model.input->virt_addr) throw std::runtime_error("allocate RGB input failed");
        std::memset(model.input->virt_addr, 114, model.input->size);
        check(rknn_set_io_mem(model.context, model.input, &input), "bind RGB8 input");
        std::vector<std::vector<float>> buffers(io.n_output);
        std::vector<rknn_output> outputs(io.n_output);
        for (uint32_t i = 0; i < io.n_output; ++i)
        {
            rknn_tensor_attr attr{};
            attr.index = i;
            check(rknn_query(model.context, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)), "query output");
            buffers[i].resize(attr.n_elems);
            outputs[i] = {};
            outputs[i].index = i;
            outputs[i].want_float = 1;
            outputs[i].is_prealloc = 1;
            outputs[i].buf = buffers[i].data();
            outputs[i].size = buffers[i].size() * sizeof(float);
        }
        double run = 0, get = 0;
        for (int i = -3; i < iterations; ++i)
        {
            npu::Execution execution(RKNN_NPU_CORE_0);
            auto start = std::chrono::steady_clock::now();
            check(execution.run(model.context), "run");
            const double run_ms = milliseconds(start);
            start = std::chrono::steady_clock::now();
            npu::Outputs lease(execution, model.context, io.n_output, outputs.data());
            check(lease.get(), "get");
            const double get_ms = milliseconds(start);
            check(lease.release(), "release");
            if (i >= 0) { run += run_ms; get += get_ms; }
        }
        std::printf("PROFILE MODE, core 0, RGB8 DMA: run=%.3f ms, get=%.3f ms (%d samples after 3 warmups)\n",
                    run / iterations, get / iterations, iterations);
        rknn_perf_detail detail{};
        check(rknn_query(model.context, RKNN_QUERY_PERF_DETAIL, &detail, sizeof(detail)), "query profile");
        if (detail.perf_data && detail.data_len) std::fwrite(detail.perf_data, 1, detail.data_len, stdout);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
