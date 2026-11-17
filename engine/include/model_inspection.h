/** @file model_inspection.h
 * @brief RKNN metadata and decoder compatibility checks, without running inference.
 * Link Vision::ModelInspection (also included by Vision::Inference).
 * No vendor, OpenCV, JSON, video or application headers are needed by callers.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vision
{
enum class TensorLayout
{
    Unknown,
    NCHW,
    NHWC,
    NC1HWC2
};
enum class TensorDataType
{
    Unknown = -1,
    Float32 = 0,
    Float16,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    Bool
};
enum class TensorQuantization
{
    Unknown = -1,
    None,
    DynamicFixedPoint,
    AffineAsymmetric
};

struct TensorMetadata
{
    std::string name;
    std::vector<std::int64_t> shape;
    TensorLayout layout = TensorLayout::Unknown;
    TensorDataType type = TensorDataType::Unknown;
    TensorQuantization quantization = TensorQuantization::Unknown;
    std::uint64_t elements = 0;
    float scale = 0;
    std::int32_t zero_point = 0;
    int fractional_length = 0;
};

struct ModelMetadata
{
    std::string path;
    std::vector<TensorMetadata> inputs;
    std::vector<TensorMetadata> outputs;
};

struct ModelInspectionResult
{
    bool ok = false;
    std::string error;
    ModelMetadata metadata;
};

struct ModelCompatibilityResult
{
    bool ok = false;
    std::string error;
    int input_width = 0;
    int input_height = 0;
    int input_channels = 0;
    TensorLayout input_layout = TensorLayout::Unknown;
};

struct ModelCheckResult
{
    ModelInspectionResult inspection;
    ModelCompatibilityResult compatibility;
    /** @brief 仅当元数据探测和解码器兼容检查均成功时返回 true。
     * 示例：
     * @code
     * const auto check = vision::check_model_file("model.rknn", "yolov8_det");
     * bool ok = check.ok();
     * @endcode
     */
    bool ok() const
    {
        return inspection.ok && compatibility.ok;
    }
    /** @brief 借用失败阶段的错误文本；检查成功时通常为空，不跨结果对象生命周期保存引用。
     * 示例：
     * @code
     * const auto check = vision::check_model_file("model.rknn", "yolov8_det");
     * if (!check.ok())
     * {
     *     std::string error = check.error();
     * }
     * @endcode
     */
    const std::string &error() const
    {
        return inspection.ok ? compatibility.error : inspection.error;
    }
};

/** @brief 加载临时 RKNN 上下文读取张量元数据并释放，不执行 rknn_run。
 * @return 失败时 ok=false 并带 error；需要板端 RKNN，进程内串行调用，元数据自有存储。
 * 示例：
 * @code
 * auto inspection = vision::inspect_model("model.rknn");
 * if (inspection.ok)
 * {
 *     auto count = inspection.metadata.outputs.size();
 *     (void)count;
 * }
 * @endcode
 */
ModelInspectionResult inspect_model(const std::string &path);

/** @brief 纯 CPU 检查张量结构是否适配指定解码器，不加载文件；空 model_type 只检查公共输入。
 * @return ok/error 表示结构兼容性，不代表精度、类别含义或运行性能。
 * 示例：
 * @code
 * auto status = vision::check_model_compatibility(metadata, "yolov8_det");
 * @endcode
 */
ModelCompatibilityResult check_model_compatibility(const ModelMetadata &metadata, const std::string &model_type);

/** @brief 一次探测文件，再检查所选模型类型；详细失败原因由结果的 error() 获取。
 * 示例：
 * @code
 * auto check = vision::check_model_file("model.rknn", "yolov8_det");
 * bool usable = check.ok();
 * @endcode
 */
ModelCheckResult check_model_file(const std::string &path, const std::string &model_type);

struct InferenceModelInfo
{
    std::string type;
    std::string result_kind; // detection / obb / pose / segmentation / depth
    bool requires_labels = false;
};

/** @brief 借用当前引擎支持的类型/结果种类/标签需求目录；纯 CPU、无需设备、不要修改。
 * 示例：
 * @code
 * for (const auto &model : vision::inference_model_types())
 * {
 *     std::string type = model.type;
 * }
 * @endcode
 */
const std::vector<InferenceModelInfo> &inference_model_types();
/** @brief 布局转为 NCHW/NHWC/NC1HWC2；未知值返回 UNDEFINED，字符串无需释放。
 * 示例：
 * @code
 * const char *layout = vision::tensor_layout_name(vision::TensorLayout::NHWC);
 * @endcode
 */
const char *tensor_layout_name(TensorLayout layout);
} // namespace vision
