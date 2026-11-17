/** @file inference.h
 * @brief Independent single-frame inference, using the same executor as video workers.
 * Link vision_inference; no capture, tracker, business callback or display server is started.
 */
#pragma once

#include <config_types.h>
#include <memory>
#include <model_inspection.h>
#include <types.h>

namespace vision
{
struct InferenceTiming
{
    float preprocess_ms = 0;
    float infer_ms = 0;
    float postprocess_ms = 0;
};

struct ModelInferenceStatus
{
    std::string model_id;
    std::string model_type;
    bool succeeded = false; // A successful model may return zero objects.
    std::string error;
    int input_width = 0;
    int input_height = 0;
    InferenceTiming timing;
};

struct FrameInferenceResult
{
    int source_width = 0;
    int source_height = 0;
    int coordinate_width = business_coordinates::WIDTH;
    int coordinate_height = business_coordinates::HEIGHT;
    double elapsed_ms = 0; // Wall time; model timings can overlap in parallel execution.
    std::vector<AlgoResult> results;
    std::vector<ModelInferenceStatus> models;

    /** @brief 至少一个模型成功时为 true；成功但没有检测目标仍算成功。
     * 示例：
     * @code
     * bool available = prediction.succeeded();
     * @endcode
     */
    bool succeeded() const;
    /** @brief 模型列表非空且所有模型均成功时为 true，用于识别部分失败。
     * 示例：
     * @code
     * if (!prediction.complete())
     * {
     *     for (const auto &m : prediction.models)
     *     {
     *         (void)m.error;
     *     }
     * }
     * @endcode
     */
    bool complete() const;
};

struct ImageInferenceOptions
{
    std::vector<ChannelModelConfig> models;
    InferenceRoiConfig inference_roi;
};

/** Owns the process-wide NPU lease and persistent model instances.
 * Construction throws on invalid options, a busy NPU or a model load failure.
 * infer() is synchronous and serialized per session. The input is read-only BGR
 * CV_8UC3; non-contiguous Mats are accepted. Results own their storage and may be
 * retained after the next call. Clone Mat payloads before modifying shared pixels.
 * Each call is independent: track_id remains -1 and no video/business state is used.
 */
class ImageInferenceSession
{
  public:
    /** @brief 按 options 加载模型并持有进程级 NPU 使用权；重复 infer() 复用模型。
     * @note options 配置模型 id/type/path/labels；配置无效、NPU 忙或加载失败抛异常。
     * 示例：
     * @code
     * vision::ImageInferenceSession session(options);
     * @endcode
     */
    explicit ImageInferenceSession(const ImageInferenceOptions &options);
    /** @brief 释放模型及 NPU 使用权；销毁前须等待所有 infer() 调用结束。
     * 示例：
     * @code
     * {
     *     vision::ImageInferenceSession session(options);
     * } // 离开作用域释放
     * @endcode
     */
    ~ImageInferenceSession();
    /** @brief 禁止复制构造，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = session;
     * (void)borrowed;
     * @endcode
     */
    ImageInferenceSession(const ImageInferenceSession &) = delete;
    /** @brief 禁止复制赋值，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = session;
     * (void)borrowed;
     * @endcode
     */
    ImageInferenceSession &operator=(const ImageInferenceSession &) = delete;
    /** @brief 同步推理只读 BGR CV_8UC3，允许非连续图；同一 session 内部串行执行。
     * @return 自有结果，可跨下一次调用保留；空图/错误类型抛 invalid_argument，模型失败见 models。
     * 示例：
     * @code
     * vision::FrameInferenceResult prediction = session.infer(image);
     * if (prediction.succeeded())
     *     vision::draw_inference_results(image, prediction);
     * @endcode
     */
    FrameInferenceResult infer(const cv::Mat &bgr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** @brief 把结果按业务坐标缩放并画到 bgr；空 model_id 选全部模型，否则按 ID 筛选。
 * @note 非空 CV_8UC3，否则抛 invalid_argument；多个深度结果只能以首个作底图。
 * 示例：
 * @code
 * cv::Mat annotated = image.clone();
 * vision::draw_inference_results(annotated, prediction, "detector");
 * @endcode
 */
void draw_inference_results(cv::Mat &bgr, const FrameInferenceResult &result, const std::string &model_id = "");
} // namespace vision
