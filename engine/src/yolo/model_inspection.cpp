#include "model_inspection.h"

#include "rknn_api.h"
#include <cstdio>
#include <json.h>
#include <stdexcept>
#include <vector>

namespace
{
const char *format_name(rknn_tensor_format format)
{
    switch (format)
    {
    case RKNN_TENSOR_NHWC:
        return "NHWC";
    case RKNN_TENSOR_NCHW:
        return "NCHW";
    default:
        return "UNDEFINED";
    }
}

cJSON *tensor_json(const rknn_tensor_attr &attr)
{
    cJSON *tensor = cJSON_CreateObject();
    cJSON_AddStringToObject(tensor, "name", attr.name);
    cJSON_AddStringToObject(tensor, "format", format_name(attr.fmt));
    cJSON_AddNumberToObject(tensor, "type", attr.type);
    cJSON_AddNumberToObject(tensor, "elements", attr.n_elems);
    cJSON_AddNumberToObject(tensor, "scale", attr.scale);
    cJSON *shape = cJSON_AddArrayToObject(tensor, "shape");
    for (uint32_t i = 0; i < attr.n_dims; ++i)
        cJSON_AddItemToArray(shape, cJSON_CreateNumber(attr.dims[i]));
    return tensor;
}

struct Context
{
    rknn_context value = 0;
    ~Context()
    {
        if (value)
            rknn_destroy(value);
    }
};
} // namespace

int inspect_model_cli(const std::string &path)
{
    cJSON *root = cJSON_CreateObject();
    int result = 0;
    try
    {
        Context context;
        int rc = rknn_init(&context.value, const_cast<char *>(path.c_str()), 0, 0, nullptr);
        if (rc < 0)
            throw std::runtime_error("RKNN 模型无法加载 (code=" + std::to_string(rc) + ")");
        rknn_input_output_num counts{};
        if (rknn_query(context.value, RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts)) < 0)
            throw std::runtime_error("无法读取 RKNN 输入/输出数量");
        if (counts.n_input != 1 || counts.n_output == 0 || counts.n_output > 256)
            throw std::runtime_error("当前推理引擎要求单输入、至少一个输出");
        rknn_tensor_attr input{};
        if (rknn_query(context.value, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input)) < 0)
            throw std::runtime_error("无法读取 RKNN 输入张量");
        if (input.n_dims != 4 || input.dims[0] != 1 || (input.fmt != RKNN_TENSOR_NHWC && input.fmt != RKNN_TENSOR_NCHW))
            throw std::runtime_error("输入必须为 batch=1 的四维 NHWC/NCHW 图像张量");
        const bool nhwc = input.fmt == RKNN_TENSOR_NHWC;
        const int height = input.dims[nhwc ? 1 : 2];
        const int width = input.dims[nhwc ? 2 : 3];
        const int channels = input.dims[nhwc ? 3 : 1];
        if (height <= 0 || width <= 0 || channels != 3)
            throw std::runtime_error("输入必须为有效尺寸的三通道图像");
        cJSON_AddItemToObject(root, "input", tensor_json(input));
        cJSON_AddNumberToObject(root, "width", width);
        cJSON_AddNumberToObject(root, "height", height);
        cJSON_AddNumberToObject(root, "channels", channels);
        cJSON_AddStringToObject(root, "format", format_name(input.fmt));
        cJSON *outputs = cJSON_AddArrayToObject(root, "outputs");
        for (uint32_t i = 0; i < counts.n_output; ++i)
        {
            rknn_tensor_attr attr{};
            attr.index = i;
            if (rknn_query(context.value, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) < 0)
                throw std::runtime_error("无法读取 RKNN 输出张量 " + std::to_string(i));
            cJSON_AddItemToArray(outputs, tensor_json(attr));
        }
        cJSON_AddBoolToObject(root, "ok", true);
    }
    catch (const std::exception &error)
    {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", error.what());
        result = 1;
    }
    char *json = cJSON_PrintUnformatted(root);
    // Vendor libraries also print to stdout; the prefix identifies the protocol line.
    printf("MODEL_INFO_JSON=%s\n", json ? json : "{\"ok\":false,\"error\":\"serialization failed\"}");
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}
