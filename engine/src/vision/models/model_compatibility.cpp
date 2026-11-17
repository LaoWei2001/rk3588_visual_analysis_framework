#include <algorithm>
#include <cmath>
#include <limits>
#include <model_inspection.h>

namespace vision
{
const std::vector<InferenceModelInfo> &inference_model_types()
{
    static const std::vector<InferenceModelInfo> types = {
#define VISION_MODEL(type, kind, labels, factory) {type, kind, labels},
#include "model_catalog.inc"
#undef VISION_MODEL
    };
    return types;
}

const char *tensor_layout_name(TensorLayout layout)
{
    switch (layout)
    {
    case TensorLayout::NCHW:
        return "NCHW";
    case TensorLayout::NHWC:
        return "NHWC";
    case TensorLayout::NC1HWC2:
        return "NC1HWC2";
    default:
        return "UNDEFINED";
    }
}

namespace
{
bool valid_shape(const TensorMetadata &tensor)
{
    if (tensor.shape.empty())
        return false;
    std::uint64_t elements = 1;
    for (auto dim : tensor.shape)
    {
        if (dim <= 0 || elements > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(dim))
            return false;
        elements *= dim;
    }
    return elements == tensor.elements;
}
bool supported_scalar(const TensorMetadata &tensor)
{
    return tensor.type == TensorDataType::Float32 || tensor.type == TensorDataType::Float16 ||
           tensor.type == TensorDataType::Int8 || tensor.type == TensorDataType::UInt8;
}
bool nchw(const TensorMetadata &tensor, const std::vector<std::int64_t> &shape)
{
    return tensor.layout == TensorLayout::NCHW && tensor.shape == shape;
}
} // namespace

ModelCompatibilityResult check_model_compatibility(const ModelMetadata &metadata, const std::string &type)
{
    ModelCompatibilityResult result;
    const auto fail = [&](const std::string &error) {
        result.error = error;
        return result;
    };
    const auto &types = inference_model_types();
    if (!type.empty() &&
        std::none_of(types.begin(), types.end(), [&](const InferenceModelInfo &info) { return info.type == type; }))
        return fail("不支持的模型类型: " + type);
    if (metadata.inputs.size() != 1 || metadata.outputs.empty() || metadata.outputs.size() > 256)
        return fail("当前推理引擎要求单输入、至少一个输出");
    const auto &input = metadata.inputs.front();
    if (input.shape.size() != 4 || input.shape[0] != 1 ||
        (input.layout != TensorLayout::NHWC && input.layout != TensorLayout::NCHW))
        return fail("输入必须为 batch=1 的四维 NHWC/NCHW 图像张量");
    if (!valid_shape(input))
        return fail("输入元素数量与张量形状不匹配");
    const bool nhwc = input.layout == TensorLayout::NHWC;
    const auto h = input.shape[nhwc ? 1 : 2], w = input.shape[nhwc ? 2 : 3], c = input.shape[nhwc ? 3 : 1];
    if (h > std::numeric_limits<int>::max() || w > std::numeric_limits<int>::max() || c != 3)
        return fail("输入必须为有效尺寸的三通道图像");
    result.input_width = static_cast<int>(w);
    result.input_height = static_cast<int>(h);
    result.input_channels = static_cast<int>(c);
    result.input_layout = input.layout;
    const auto &outputs = metadata.outputs;
    for (const auto &output : outputs)
        if (!valid_shape(output))
            return fail("输出元素数量与张量形状不匹配");
    if (type.empty())
    {
        result.ok = true;
        return result;
    }
    if ((type == "yolov5" || type == "yolov5_seg") && !nhwc)
        return fail(type + " 当前要求 NHWC 输入布局");

    if (type == "yolo26_depth" || type == "zip_depth")
    {
        if (outputs.size() != 1)
            return fail(type + " 要求单个深度输出");
        const auto &output = outputs[0];
        const auto &shape = output.shape;
        std::int64_t sh = 0, sw = 0;
        if (shape.size() == 4 && shape[0] == 1 && output.layout == TensorLayout::NCHW && shape[1] == 1)
        {
            sh = shape[2];
            sw = shape[3];
        }
        else if (shape.size() == 4 && shape[0] == 1 && output.layout == TensorLayout::NHWC && shape[3] == 1)
        {
            sh = shape[1];
            sw = shape[2];
        }
        else if (shape.size() == 3 && shape[0] == 1)
        {
            sh = shape[1];
            sw = shape[2];
        }
        else if (shape.size() == 2)
        {
            sh = shape[0];
            sw = shape[1];
        }
        else
            return fail(type + " 输出必须为单通道 [1,1,H,W] / [1,H,W,1] / [1,H,W] / [H,W]");
        if (sh > h || sw > w || sh * w != sw * h)
            return fail(type + " 输出必须与输入画布宽高比一致，可为较低分辨率深度图");
        if (!supported_scalar(output))
            return fail(type + " 输出仅支持 FLOAT32/FLOAT16/INT8/UINT8");
    }
    else if (type == "yolo26_pose")
    {
        std::vector<std::int64_t> shape;
        if (outputs.size() == 1)
            for (auto dim : outputs[0].shape)
                if (dim != 1)
                    shape.push_back(dim);
        if (shape.size() != 2 || (shape[0] != 56 && shape[1] != 56))
            return fail("yolo26_pose 要求单输出，特征维为 56（17 个关键点）");
        if (!supported_scalar(outputs[0]))
            return fail("yolo26_pose 输出仅支持 FLOAT32/FLOAT16/INT8/UINT8");
        if ((outputs[0].type == TensorDataType::Int8 || outputs[0].type == TensorDataType::UInt8) &&
            (!std::isfinite(outputs[0].scale) || outputs[0].scale <= 0 || outputs[0].scale > 1))
            return fail("yolo26_pose INT8 输出量化 scale 必须在 (0, 1] 内，否则无法保留置信度，需要重新导出模型");
    }
    else
    {
        if (w % 32 || h % 32)
            return fail(type + " 当前要求输入宽高为 32 的整数倍");
        const int strides[] = {8, 16, 32};
        if (type == "yolov8_obb")
        {
            if (outputs.size() != 4)
                return fail("yolov8_obb 要求 3 个 DFL 检测头及 1 个角度输出");
            const auto &shape = outputs[0].shape;
            const auto classes = shape.size() == 4 ? shape[1] - 64 : 0;
            if (classes <= 0)
                return fail("yolov8_obb 检测头通道数必须为 64 + 类别数");
            std::uint64_t cells = 0;
            for (int i = 0; i < 3; ++i)
            {
                if (!nchw(outputs[i], {1, 64 + classes, h / strides[i], w / strides[i]}))
                    return fail("yolov8_obb 输出布局或步长不兼容");
                cells += (h / strides[i]) * (w / strides[i]);
            }
            if (outputs[3].elements != cells)
                return fail("yolov8_obb 角度输出大小不匹配");
        }
        else if (type == "yolov8_det")
        {
            if (outputs.size() != 9)
                return fail("yolov8_det 要求三个检测分支、共 9 个输出张量");
            for (int i = 0; i < 3; ++i)
            {
                const auto &box = outputs[i * 3], &score = outputs[i * 3 + 1];
                if (box.shape.size() != 4 || score.shape.size() != 4 || box.shape[1] < 4 || box.shape[1] % 4 ||
                    !nchw(box, {1, box.shape[1], h / strides[i], w / strides[i]}) ||
                    !nchw(score, {1, score.shape[1], h / strides[i], w / strides[i]}))
                    return fail("yolov8_det 输出布局或检测网格与当前解码器不兼容");
            }
        }
        else if (type == "yolov5" || type == "yolov5_seg")
        {
            const std::size_t count = type == "yolov5" ? 3 : 7;
            if (outputs.size() != count)
                return fail(type + " 当前要求 " + std::to_string(count) + " 个输出张量");
            for (int i = 0; i < 3; ++i)
            {
                const auto index = count == 3 ? i : i * 2;
                const auto &head = outputs[index];
                if (head.shape.size() != 4 || head.shape[1] <= 15 || head.shape[1] % 3 ||
                    !nchw(head, {1, head.shape[1], h / strides[i], w / strides[i]}))
                    return fail(type + " 检测头输出布局与当前解码器不兼容");
                if (count == 7 && !nchw(outputs[index + 1], {1, 96, h / strides[i], w / strides[i]}))
                    return fail("yolov5_seg 要求每个检测头对应 32 维 mask 系数");
            }
            if (count == 7)
            {
                const auto &proto = outputs[6];
                if (proto.layout != TensorLayout::NCHW || proto.shape.size() != 4 || proto.shape[0] != 1 ||
                    proto.shape[1] != 32 || proto.shape[2] > h || proto.shape[3] > w)
                    return fail("yolov5_seg 要求 NCHW 的 32 通道有效 proto 输出");
            }
        }
        else if (type == "yolov8_pose")
        {
            if (outputs.size() < 4)
                return fail("yolov8_pose 要求三个检测头和关键点输出");
            std::uint64_t candidates = 0;
            for (int i = 0; i < 3; ++i)
            {
                const auto &head = outputs[i];
                if (head.layout != TensorLayout::NCHW || head.shape.size() != 4 || head.shape[0] != 1 ||
                    head.shape[1] != 65)
                    return fail("yolov8_pose 当前要求 NCHW 的 65 通道检测头");
                // valid_shape() guarantees the element count is representable.
                candidates += head.elements / 65;
            }
            if (candidates > std::numeric_limits<std::uint64_t>::max() / 3 ||
                std::none_of(outputs.begin() + 3, outputs.end(),
                             [&](const TensorMetadata &out) { return out.elements % (candidates * 3) == 0; }))
                return fail("yolov8_pose 关键点输出数量与检测候选数不匹配");
        }
    }
    result.ok = true;
    return result;
}
} // namespace vision
