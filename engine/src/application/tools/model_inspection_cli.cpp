#include "model_inspection_cli.h"
#include <cstdio>
#include <json.h>
#include <model_inspection.h>

namespace
{
cJSON *tensor_json(const vision::TensorMetadata &tensor)
{
    auto value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "name", tensor.name.c_str());
    cJSON_AddStringToObject(value, "format", vision::tensor_layout_name(tensor.layout));
    cJSON_AddNumberToObject(value, "type", static_cast<int>(tensor.type));
    cJSON_AddNumberToObject(value, "elements", tensor.elements);
    cJSON_AddNumberToObject(value, "scale", tensor.scale);
    cJSON_AddNumberToObject(value, "zero_point", tensor.zero_point);
    cJSON_AddNumberToObject(value, "quantization", static_cast<int>(tensor.quantization));
    cJSON_AddNumberToObject(value, "fractional_length", tensor.fractional_length);
    auto shape = cJSON_AddArrayToObject(value, "shape");
    for (auto dim : tensor.shape)
        cJSON_AddItemToArray(shape, cJSON_CreateNumber(dim));
    return value;
}
} // namespace

int inspect_model_cli(const std::string &path, const std::string &model_type)
{
    const auto checked = vision::check_model_file(path, model_type);
    const auto &metadata = checked.inspection.metadata;
    auto root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "inspection_version", 1);
    cJSON_AddBoolToObject(root, "ok", checked.ok());
    if (!checked.ok())
        cJSON_AddStringToObject(root, "error", checked.error().c_str());
    if (checked.inspection.ok)
    {
        const auto &input = checked.compatibility;
        cJSON_AddNumberToObject(root, "width", input.input_width);
        cJSON_AddNumberToObject(root, "height", input.input_height);
        cJSON_AddNumberToObject(root, "channels", input.input_channels);
        cJSON_AddStringToObject(root, "format", vision::tensor_layout_name(input.input_layout));
        if (!metadata.inputs.empty())
            cJSON_AddItemToObject(root, "input", tensor_json(metadata.inputs[0]));
        auto inputs = cJSON_AddArrayToObject(root, "inputs");
        for (const auto &tensor : metadata.inputs)
            cJSON_AddItemToArray(inputs, tensor_json(tensor));
        auto outputs = cJSON_AddArrayToObject(root, "outputs");
        for (const auto &tensor : metadata.outputs)
            cJSON_AddItemToArray(outputs, tensor_json(tensor));
        auto compatibility = cJSON_AddObjectToObject(root, "compatibility");
        for (const auto &info : vision::inference_model_types())
        {
            const auto status = vision::check_model_compatibility(metadata, info.type);
            auto entry = cJSON_AddObjectToObject(compatibility, info.type.c_str());
            cJSON_AddBoolToObject(entry, "ok", status.ok);
            cJSON_AddStringToObject(entry, "error", status.error.c_str());
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    // Vendor libraries print logs too; this prefix identifies our response.
    std::printf("MODEL_INFO_JSON=%s\n", json ? json : "{\"ok\":false,\"error\":\"serialization failed\"}");
    const bool success = checked.ok() && json;
    cJSON_free(json);
    cJSON_Delete(root);
    return success ? 0 : 1;
}
