#include <cstring>
#include <model_inspection.h>
#include <mutex>
#include <rknn_api.h>
#include <stdexcept>

namespace vision
{
namespace
{
std::mutex probe_mutex;
struct Context
{
    rknn_context value = 0;
    ~Context()
    {
        if (value)
            rknn_destroy(value);
    }
};

TensorMetadata convert(const rknn_tensor_attr &attr)
{
    if (attr.n_dims > RKNN_MAX_DIMS)
        throw std::runtime_error("RKNN 张量维度数量无效");
    TensorMetadata tensor;
    tensor.name.assign(attr.name, strnlen(attr.name, sizeof(attr.name)));
    tensor.shape.assign(attr.dims, attr.dims + attr.n_dims);
    switch (attr.fmt)
    {
    case RKNN_TENSOR_NCHW:
        tensor.layout = TensorLayout::NCHW;
        break;
    case RKNN_TENSOR_NHWC:
        tensor.layout = TensorLayout::NHWC;
        break;
    case RKNN_TENSOR_NC1HWC2:
        tensor.layout = TensorLayout::NC1HWC2;
        break;
    default:
        break;
    }
    switch (attr.type)
    {
    case RKNN_TENSOR_FLOAT32:
        tensor.type = TensorDataType::Float32;
        break;
    case RKNN_TENSOR_FLOAT16:
        tensor.type = TensorDataType::Float16;
        break;
    case RKNN_TENSOR_INT8:
        tensor.type = TensorDataType::Int8;
        break;
    case RKNN_TENSOR_UINT8:
        tensor.type = TensorDataType::UInt8;
        break;
    case RKNN_TENSOR_INT16:
        tensor.type = TensorDataType::Int16;
        break;
    case RKNN_TENSOR_UINT16:
        tensor.type = TensorDataType::UInt16;
        break;
    case RKNN_TENSOR_INT32:
        tensor.type = TensorDataType::Int32;
        break;
    case RKNN_TENSOR_UINT32:
        tensor.type = TensorDataType::UInt32;
        break;
    case RKNN_TENSOR_INT64:
        tensor.type = TensorDataType::Int64;
        break;
    case RKNN_TENSOR_BOOL:
        tensor.type = TensorDataType::Bool;
        break;
    default:
        break;
    }
    switch (attr.qnt_type)
    {
    case RKNN_TENSOR_QNT_NONE:
        tensor.quantization = TensorQuantization::None;
        break;
    case RKNN_TENSOR_QNT_DFP:
        tensor.quantization = TensorQuantization::DynamicFixedPoint;
        break;
    case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:
        tensor.quantization = TensorQuantization::AffineAsymmetric;
        break;
    default:
        break;
    }
    tensor.elements = attr.n_elems;
    tensor.scale = attr.scale;
    tensor.zero_point = attr.zp;
    tensor.fractional_length = attr.fl;
    return tensor;
}
} // namespace

ModelInspectionResult inspect_model(const std::string &path)
{
    ModelInspectionResult result;
    result.metadata.path = path;
    try
    {
        if (path.empty() || path.find('\0') != std::string::npos)
            throw std::invalid_argument("模型路径为空或包含空字符");
        std::lock_guard<std::mutex> lock(probe_mutex);
        Context context;
        const int rc = rknn_init(&context.value, const_cast<char *>(path.c_str()), 0, 0, nullptr);
        if (rc < 0)
            throw std::runtime_error("RKNN 模型无法加载 (code=" + std::to_string(rc) + ")");
        rknn_input_output_num counts{};
        if (rknn_query(context.value, RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts)) < 0)
            throw std::runtime_error("无法读取 RKNN 输入/输出数量");
        if (!counts.n_input || !counts.n_output || counts.n_input > 256 || counts.n_output > 256)
            throw std::runtime_error("RKNN 输入/输出张量数量无效");
        for (uint32_t i = 0; i < counts.n_input; ++i)
        {
            rknn_tensor_attr attr{};
            attr.index = i;
            if (rknn_query(context.value, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr)) < 0)
                throw std::runtime_error("无法读取 RKNN 输入张量 " + std::to_string(i));
            result.metadata.inputs.push_back(convert(attr));
        }
        for (uint32_t i = 0; i < counts.n_output; ++i)
        {
            rknn_tensor_attr attr{};
            attr.index = i;
            if (rknn_query(context.value, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) < 0)
                throw std::runtime_error("无法读取 RKNN 输出张量 " + std::to_string(i));
            result.metadata.outputs.push_back(convert(attr));
        }
        result.ok = true;
    }
    catch (const std::exception &error)
    {
        result.error = error.what();
    }
    return result;
}

ModelCheckResult check_model_file(const std::string &path, const std::string &model_type)
{
    ModelCheckResult result;
    result.inspection = inspect_model(path);
    if (result.inspection.ok)
        result.compatibility = check_model_compatibility(result.inspection.metadata, model_type);
    return result;
}
} // namespace vision
