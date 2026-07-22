#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include "rknn_api.h"
#include "yolo_utils.h"
#include "model_base.h"

class YOLO : public ModelBase {
public:
    YOLO(const std::string& model_path, const std::string& label_path, int core_mask = RKNN_NPU_CORE_0_1_2,
         float obj_thresh = 0.4f, float nms_thresh = 0.45f);
    virtual ~YOLO();

    virtual bool infer(cv::Mat& frame, std::vector<AlgoResult>& results, YoloPerfStat* perf = nullptr) override;

    virtual int get_input_fd() const override { return in_mem_ ? in_mem_->fd : -1; }
    virtual int get_input_rga_handle() const override { return input_rga_handle_; }
    virtual bool infer_zero_copy(std::vector<AlgoResult>& results, YoloPerfStat* perf = nullptr) override;

    virtual int input_width() const override { return model_w_; }
    virtual int input_height() const override { return model_h_; }

    virtual void set_thresh(float obj_thresh, float nms_thresh) override {
        obj_thresh_ = obj_thresh;
        nms_thresh_ = nms_thresh;
    }
    virtual float get_obj_thresh() const override { return obj_thresh_; }

private:
    void init_rknn(const std::string& model_path, int core_mask);
    void load_labels(const std::string& path);
    void query_model_info();
    bool init_zero_copy_input();

    cv::Mat preprocess(cv::Mat& img, LetterBoxInfo& lb);
    void post_process(void* feat, int out_idx, int feat_h, int feat_w,
                     LetterBoxInfo& lb, std::vector<AlgoResult>& results);

private:
    bool is_quant_ = false;
    rknn_context ctx_ = 0;
    rknn_tensor_attr in_attr_{};
    rknn_tensor_attr in_io_attr_{};
    rknn_tensor_mem* in_mem_ = nullptr;
    uint32_t in_copy_size_ = 0;
    bool zero_copy_input_enabled_ = false;
    int model_w_ = 640;
    int model_h_ = 640;
    int num_classes_ = 80;
    int io_num_in_ = 0;
    int io_num_out_ = 0;
    std::vector<rknn_tensor_attr> out_attrs_;
    std::vector<std::string> labels_;

    float obj_thresh_ = 0.4f;
    float nms_thresh_ = 0.45f;

    /* ---- 性能优化：热路径缓存，消除动态内存分配 ---- */
    std::vector<rknn_output> rknn_outputs_cache_;
    std::vector<AlgoResult>  candidates_cache_;

    /* 预缓存的 RGA dst handle (rga_buffer_handle_t = int).
     * init_zero_copy_input 成功后 import 一次, ~YOLO 中 release.
     * 0 = 未初始化 / import 失败, worker 走每帧 import 路径. */
    int input_rga_handle_ = 0;
};