#pragma once
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include "rknn_api.h"
#include "yolo_utils.h"
#include "model_base.h"

class YoloV8Det : public ModelBase {
public:
    YoloV8Det(const std::string& model_path, const std::string& label_path, int core_mask = RKNN_NPU_CORE_0_1_2,
         float obj_thresh = 0.25f, float nms_thresh = 0.45f);
    virtual ~YoloV8Det();

    virtual bool infer(cv::Mat& frame, std::vector<AlgoResult>& results, YoloPerfStat* perf = nullptr) override;

    /* ---- 零拷贝接口: 与 YOLO (v5) 完全对称, 让 algoProcess 走 RGA 直写 NPU 路径 ---- */
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

    virtual bool nms_done() const override { return true; }

private:
    void init_rknn(const std::string& model_path, int core_mask);
    void load_labels(const std::string& path);
    void query_model_info();
    bool init_zero_copy_input();

    cv::Mat preprocess(cv::Mat& img, LetterBoxInfo& lb);
    int post_process(rknn_output* outputs, LetterBoxInfo& lb, std::vector<AlgoResult>& results);

    int process_i8(int8_t* box_tensor, int32_t box_zp, float box_scale,
                   int8_t* score_tensor, int32_t score_zp, float score_scale,
                   int8_t* score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                                     int grid_h, int grid_w, int stride, int dfl_len, int class_num,
                   std::vector<float>& boxes, std::vector<float>& objProbs, std::vector<int>& classId, float threshold);

    int process_fp32(float* box_tensor, float* score_tensor, float* score_sum_tensor,
                                         int grid_h, int grid_w, int stride, int dfl_len, int class_num,
                     std::vector<float>& boxes, std::vector<float>& objProbs, std::vector<int>& classId, float threshold);

    void compute_dfl(float* tensor, int dfl_len, float* box);
    int nms(int validCount, std::vector<float>& outputLocations, std::vector<int>& classIds,
            std::vector<int>& order, int filterId, float threshold);
    void quick_sort_indice_inverse(std::vector<float>& input, int left, int right, std::vector<int>& indices);

private:
    rknn_context ctx_ = 0;
    int model_w_ = 640;
    int model_h_ = 640;
    int num_classes_ = 80;
    int io_num_in_ = 0;
    int io_num_out_ = 0;
    std::vector<rknn_tensor_attr> out_attrs_;
    std::vector<std::string> labels_;
    bool is_quant_ = true;

    float obj_thresh_ = 0.25f;
    float nms_thresh_ = 0.45f;

    /* ---- 零拷贝输入相关 ---- */
    rknn_tensor_attr in_attr_{};        // 模型查询到的输入属性
    rknn_tensor_attr in_io_attr_{};     // set_io_mem 用的属性 (UINT8 / NHWC)
    rknn_tensor_mem* in_mem_ = nullptr; // RKNN 分配的输入缓冲, fd 暴露给 RGA
    uint32_t in_copy_size_ = 0;
    bool zero_copy_input_enabled_ = false;

    /* ---- 推理热路径输出缓存 ---- */
    std::vector<rknn_output> rknn_outputs_cache_;

    /* 预缓存 RGA dst handle, 0=未初始化/失败 */
    int input_rga_handle_ = 0;
};
