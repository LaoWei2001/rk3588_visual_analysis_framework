#pragma once

#include "model_base.h"
#include "rknn_api.h"

#include <cstddef>
#include <cstdint>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

/**
 * YOLO26 Pose（Ultralytics one-to-many RKNN 导出）解码器。
 *
 * 与旧 YoloPose 的“3 个 DFL 检测头 + 独立关键点张量”不同，本类只处理
 * [1, 56, N] / [1, N, 56] 单输出：4 个 xywh、1 个 person 概率、17×3
 * 个关键点。单独成类可以让两种模型的布局检查和热路径互不干扰。
 */
class Yolo26Pose final : public ModelBase
{
  public:
    Yolo26Pose(const std::string &model_path, const std::string &label_path, int core_mask = RKNN_NPU_CORE_0_1_2,
               float obj_thresh = 0.25f, float nms_thresh = 0.45f);
    ~Yolo26Pose() override;

    bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;
    bool infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;

    int get_input_fd() const override
    {
        return input_mem_ ? input_mem_->fd : -1;
    }
    int get_input_rga_handle() const override
    {
        return input_rga_handle_;
    }
    int input_width() const override
    {
        return model_width_;
    }
    int input_height() const override
    {
        return model_height_;
    }
    void set_thresh(float obj_thresh, float nms_thresh) override
    {
        obj_thresh_ = obj_thresh;
        nms_thresh_ = nms_thresh;
    }
    float get_obj_thresh() const override
    {
        return obj_thresh_;
    }
    bool nms_done() const override
    {
        return true;
    }

  private:
    struct Letterbox
    {
        float scale = 1.0f;
        int pad_x = 0;
        int pad_y = 0;
    };

    struct Candidate
    {
        cv::Rect2f box;
        float score = 0.0f;
        int tensor_index = -1;
    };

    void initialize(const std::string &model_path, int core_mask);
    void query_model();
    void configure_output();
    void load_label(const std::string &label_path);
    bool initialize_zero_copy_input();

    cv::Mat &preprocess(const cv::Mat &image, Letterbox &letterbox);
    bool run_and_decode(const Letterbox &letterbox, int image_width, int image_height, std::vector<AlgoResult> &results,
                        YoloPerfStat *perf, float preprocess_ms);
    void decode(const Letterbox &letterbox, int image_width, int image_height, const void *output,
                std::vector<AlgoResult> &results);
    float output_value(const void *output, int candidate, int feature) const;

    static float half_to_float(uint16_t value);
    static float probability(float value);
    static float box_iou(const cv::Rect2f &a, const cv::Rect2f &b);

    rknn_context context_ = 0;
    rknn_tensor_attr input_attr_{};
    rknn_tensor_attr input_io_attr_{};
    rknn_tensor_attr output_attr_{};

    int model_width_ = 0;
    int model_height_ = 0;
    int model_channels_ = 0;
    int candidate_count_ = 0;
    bool features_first_ = true;

    static constexpr int kClassCount = 1;
    static constexpr int kKeypointCount = 17;
    static constexpr int kFeatureCount = 4 + kClassCount + kKeypointCount * 3;
    static constexpr int kMaxDetections = 100;

    std::string label_ = "person";
    float obj_thresh_ = 0.25f;
    float nms_thresh_ = 0.45f;

    rknn_tensor_mem *input_mem_ = nullptr;
    int input_rga_handle_ = 0;
    bool zero_copy_enabled_ = false;

    /* 热路径缓存：输出保持原生 FP16，避免 RKNN 每帧分配并转换 470400 个 float。 */
    std::vector<uint8_t> native_output_;
    std::vector<Candidate> candidates_;
    std::vector<Candidate> kept_;
    cv::Mat resized_;
    cv::Mat rgb_input_;
};
