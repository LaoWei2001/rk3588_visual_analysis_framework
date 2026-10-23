#pragma once

#include "model_base.h"
#include <rknn_api.h>

// YOLO26-Depth export contract from jlu-lujing/yolo26depth-rknn (MIT).
// RGB uint8 input; exp/clamp and metric-depth conversion are baked into RKNN.
class Yolo26Depth final : public ModelBase
{
  public:
    explicit Yolo26Depth(const std::string &path, int core_mask = RKNN_NPU_CORE_AUTO);
    ~Yolo26Depth() override;
    bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;
    bool infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;
    int get_input_fd() const override { return input_mem_ ? input_mem_->fd : -1; }
    int get_input_rga_handle() const override { return input_rga_handle_; }
    int input_width() const override { return width_; }
    int input_height() const override { return height_; }
    void set_thresh(float, float) override {}
    float get_obj_thresh() const override { return 0.0f; }
    float get_nms_thresh() const override { return 1.0f; }
    bool nms_done() const override { return true; }

  private:
    void query_model();
    void init_input_memory();
    void release();
    bool run(std::vector<AlgoResult> &results, YoloPerfStat *perf, float preprocess_ms);
    cv::Mat acquire_output();

    rknn_context context_ = 0;
    rknn_tensor_mem *input_mem_ = nullptr;
    rknn_tensor_attr input_attr_{};
    int input_rga_handle_ = 0;
    int width_ = 0, height_ = 0, output_width_ = 0, output_height_ = 0;
    cv::Mat resized_, rgb_;
    // Only reuse slots with no published readers. External snapshots can retain
    // pixels for arbitrarily long without being overwritten by a later frame.
    std::vector<cv::Mat> output_pool_;
};
