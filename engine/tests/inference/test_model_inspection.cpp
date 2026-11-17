#include <cassert>
#include <iostream>
#include <limits>
#include <model_inspection.h>

using namespace vision;
namespace
{
TensorMetadata tensor(std::vector<std::int64_t> shape, TensorLayout layout = TensorLayout::NCHW,
                      TensorDataType type = TensorDataType::Float32)
{
    TensorMetadata out;
    out.shape = std::move(shape);
    out.layout = layout;
    out.type = type;
    out.elements = 1;
    out.scale = .1f;
    for (auto dim : out.shape)
        out.elements *= dim;
    return out;
}
ModelMetadata input(int w = 640, int h = 640, TensorLayout layout = TensorLayout::NHWC)
{
    ModelMetadata out;
    out.inputs.push_back(tensor(layout == TensorLayout::NHWC ? std::vector<std::int64_t>{1, h, w, 3}
                                                             : std::vector<std::int64_t>{1, 3, h, w},
                                layout));
    return out;
}
ModelMetadata detector(int w = 640, int h = 640)
{
    auto out = input(w, h);
    for (int stride : {8, 16, 32})
        for (int channels : {64, 2, 1})
            out.outputs.push_back(tensor({1, channels, h / stride, w / stride}));
    return out;
}
void good(const ModelMetadata &model, const std::string &type)
{
    const auto result = check_model_compatibility(model, type);
    if (!result.ok)
        std::cerr << type << ": " << result.error << '\n';
    assert(result.ok && result.error.empty() && result.input_width > 0 && result.input_channels == 3);
}
void bad(const ModelMetadata &model, const std::string &type, const std::string &message = "")
{
    const auto result = check_model_compatibility(model, type);
    assert(!result.ok && !result.error.empty());
    if (!message.empty())
        assert(result.error.find(message) != std::string::npos);
}
void common_and_detection()
{
    for (const auto size : {640, 960})
        good(detector(size, size), "yolov8_det");
    good(detector(768, 1088), "yolov8_det");
    good(detector(), "");
    bad(detector(), "unknown");
    bad(detector(), "yolov5", "3 个输出");
    auto model = detector();
    model.inputs[0] = tensor({1, 3, 640, 640}, TensorLayout::NCHW);
    good(model, "yolov8_det");
    bad(model, "yolov5", "NHWC");
    model = detector();
    model.inputs.push_back(model.inputs.front());
    bad(model, "yolov8_det", "单输入");
    model = detector();
    model.inputs.clear();
    bad(model, "yolov8_det");
    model = detector();
    model.inputs[0] = tensor({2, 640, 640, 3}, TensorLayout::NHWC);
    bad(model, "yolov8_det", "batch");
    model = detector();
    model.inputs[0] = tensor({1, 640, 640, 1}, TensorLayout::NHWC);
    bad(model, "yolov8_det", "三通道");
    model = detector();
    model.inputs[0].layout = TensorLayout::NC1HWC2;
    bad(model, "yolov8_det");
    model = detector();
    model.inputs[0].elements++;
    bad(model, "yolov8_det", "元素");
    model = detector();
    model.inputs[0] = tensor({1, 640, 641, 3}, TensorLayout::NHWC);
    bad(model, "yolov8_det", "32");
    model = detector();
    model.inputs[0] = tensor({1, 640, 10000000000LL, 3}, TensorLayout::NHWC);
    bad(model, "yolov8_det");
    model = detector();
    model.outputs.clear();
    bad(model, "yolov8_det");
    model = detector();
    model.outputs.pop_back();
    bad(model, "yolov8_det", "9 个输出");
    model = detector();
    model.outputs[0] = tensor({1, 64, 79, 80});
    bad(model, "yolov8_det", "网格");
    model = detector();
    model.outputs[0] = tensor({1, 5, 80, 80});
    bad(model, "yolov8_det");
    model = detector();
    model.outputs[1] = tensor({2, 2, 80, 80});
    bad(model, "yolov8_det");
    model = detector();
    model.outputs[0].elements++;
    bad(model, "yolov8_det", "元素");
    model = detector();
    model.outputs[0].shape = {};
    bad(model, "yolov8_det");
    model = detector();
    model.outputs[0].shape[2] = -1;
    bad(model, "yolov8_det");
    model = detector();
    model.outputs[0].shape = {std::numeric_limits<std::int64_t>::max(), 10};
    bad(model, "yolov8_det");
}
void v5_and_segmentation()
{
    for (const auto size : {640, 960, 1088})
    {
        auto model = input(768, size);
        for (int stride : {8, 16, 32})
            model.outputs.push_back(tensor({1, 255, size / stride, 768 / stride}));
        good(model, "yolov5");
        auto broken = model;
        broken.outputs[0] = tensor({1, 16, size / 8, 96});
        bad(broken, "yolov5");
        model.outputs.clear();
        for (int stride : {8, 16, 32})
            for (int channels : {255, 96})
                model.outputs.push_back(tensor({1, channels, size / stride, 768 / stride}));
        model.outputs.push_back(tensor({1, 32, size / 4, 192}));
        good(model, "yolov5_seg");
        model.outputs[6] = tensor({1, 32, 160, 160});
        good(model, "yolov5_seg");
        broken = model;
        broken.outputs[6] = tensor({1, 31, 160, 160});
        bad(broken, "yolov5_seg", "proto");
        broken = model;
        broken.outputs[1] = tensor({1, 95, size / 8, 96});
        bad(broken, "yolov5_seg", "系数");
        broken = model;
        broken.outputs[6] = tensor({1, 32, size + 1, 160});
        bad(broken, "yolov5_seg", "proto");
        broken = model;
        broken.outputs[6].shape[2] = 0;
        bad(broken, "yolov5_seg");
    }
}
void obb_and_pose()
{
    auto model = input();
    for (int stride : {8, 16, 32})
        model.outputs.push_back(tensor({1, 79, 640 / stride, 640 / stride}));
    model.outputs.push_back(tensor({1, 1, 8400}));
    good(model, "yolov8_obb");
    auto broken = model;
    broken.outputs[3] = tensor({1, 1, 8399});
    bad(broken, "yolov8_obb", "角度");
    broken = model;
    broken.outputs[1] = tensor({1, 80, 40, 40});
    bad(broken, "yolov8_obb");
    broken = model;
    broken.outputs[0] = tensor({1, 64, 80, 80});
    bad(broken, "yolov8_obb", "类别数");
    model.outputs.clear();
    for (int stride : {8, 16, 32})
        model.outputs.push_back(tensor({1, 65, 640 / stride, 640 / stride}));
    model.outputs.push_back(tensor({1, 51, 8400}));
    good(model, "yolov8_pose");
    broken = model;
    broken.outputs[0] = tensor({1, 64, 80, 80});
    bad(broken, "yolov8_pose", "65");
    broken = model;
    broken.outputs[3] = tensor({1, 51, 8401});
    bad(broken, "yolov8_pose", "候选数");
    broken = model;
    broken.outputs.pop_back();
    bad(broken, "yolov8_pose");
    model = input(960, 960, TensorLayout::NCHW);
    model.outputs = {tensor({1, 56, 300})};
    good(model, "yolo26_pose");
    model.outputs = {tensor({1, 300, 56})};
    good(model, "yolo26_pose");
    for (auto type : {TensorDataType::Float32, TensorDataType::Float16, TensorDataType::Int8, TensorDataType::UInt8})
    {
        model.outputs[0].type = type;
        good(model, "yolo26_pose");
    }
    model.outputs[0].type = TensorDataType::Int32;
    bad(model, "yolo26_pose", "FLOAT32");
    model.outputs[0].type = TensorDataType::Int8;
    for (float scale :
         {0.f, -1.f, 2.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        model.outputs[0].scale = scale;
        bad(model, "yolo26_pose", "scale");
    }
    model.outputs[0].scale = 1;
    good(model, "yolo26_pose");
    model.outputs[0].elements++;
    bad(model, "yolo26_pose", "元素");
    model.outputs = {tensor({1, 57, 300})};
    bad(model, "yolo26_pose", "56");
}
void depth()
{
    for (const auto &type : {"yolo26_depth", "zip_depth"})
    {
        auto model = input(640, 640, TensorLayout::NCHW);
        for (auto out : {tensor({1, 1, 640, 640}), tensor({1, 640, 640, 1}, TensorLayout::NHWC),
                         tensor({1, 160, 160}, TensorLayout::Unknown), tensor({160, 160}, TensorLayout::Unknown)})
        {
            model.outputs = {out};
            good(model, type);
            model.outputs[0].elements++;
            bad(model, type, "元素");
        }
        model = input(640, 352);
        model.outputs = {tensor({1, 176, 320, 1}, TensorLayout::NHWC)};
        good(model, type);
        auto broken = model;
        broken.outputs[0] = tensor({1, 1, 176, 321});
        bad(broken, type, "宽高比");
        broken = model;
        broken.outputs[0] = tensor({1, 1, 704, 1280});
        bad(broken, type, "宽高比");
        broken = model;
        broken.outputs[0] = tensor({1, 2, 176, 320});
        bad(broken, type, "单通道");
        broken = model;
        broken.outputs[0].type = TensorDataType::Int32;
        bad(broken, type, "FLOAT32");
        broken = model;
        broken.outputs.push_back(broken.outputs[0]);
        bad(broken, type, "单个");
        bad(detector(), type, "单个");
    }
}
} // namespace

int main()
{
    assert(inference_model_types().size() == 8);
    common_and_detection();
    v5_and_segmentation();
    obb_and_pose();
    depth();
    std::cout << "All eight decoder contracts and malformed metadata checks passed\n";
}
