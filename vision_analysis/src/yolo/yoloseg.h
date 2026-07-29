#pragma once
#include "model_base.h"
#include "rknn_api.h"
#include "yolo_utils.h"
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#define PROTO_CHANNEL 32
#define PROTO_HEIGHT 160
#define PROTO_WEIGHT 160

class YoloSeg : public ModelBase
{
  public:
    YoloSeg(const std::string &model_path, const std::string &label_path, int core_mask = RKNN_NPU_CORE_0_1_2,
            float obj_thresh = 0.4f, float nms_thresh = 0.45f);
    virtual ~YoloSeg();

    virtual bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;

    /* ---- 零拷贝接口: 与 YOLO (v5) / YoloV8Det / YoloPose 保持一致 ----
     * 注意: 零拷贝下 box 与 mask 均在模型空间 (model_w x model_h),
     *       下游 channel_logic 按"输入坐标系 = 模型尺寸"处理, 与 v5 一致. */
    virtual int get_input_fd() const override
    {
        return in_mem_ ? in_mem_->fd : -1;
    }
    virtual int get_input_rga_handle() const override
    {
        return input_rga_handle_;
    }
    virtual bool infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;

    virtual int input_width() const override
    {
        return model_w_;
    }
    virtual int input_height() const override
    {
        return model_h_;
    }

    virtual void set_thresh(float obj_thresh, float nms_thresh) override
    {
        obj_thresh_ = obj_thresh;
        nms_thresh_ = nms_thresh;
    }
    virtual float get_obj_thresh() const override
    {
        return obj_thresh_;
    }

    virtual bool nms_done() const override
    {
        return true;
    }

  private:
    void init_rknn(const std::string &model_path, int core_mask);
    void load_labels(const std::string &path);
    void query_model_info();
    bool init_zero_copy_input();

    cv::Mat preprocess(cv::Mat &img, LetterBoxInfo &lb);
    int post_process(rknn_output *outputs, LetterBoxInfo &lb, int ori_in_width, int ori_in_height,
                     std::vector<AlgoResult> &results);

    int process_i8(rknn_output *all_input, int input_id, const int *anchor, int grid_h, int grid_w, int height,
                   int width, int stride, int class_num, std::vector<float> &boxes, std::vector<float> &segments,
                   float *proto, std::vector<float> &objProbs, std::vector<int> &classId, float threshold);
    int process_fp32(rknn_output *all_input, int input_id, const int *anchor, int grid_h, int grid_w, int height,
                     int width, int stride, int class_num, std::vector<float> &boxes, std::vector<float> &segments,
                     float *proto, std::vector<float> &objProbs, std::vector<int> &classId, float threshold);

  private:
    rknn_context ctx_ = 0;
    rknn_tensor_attr in_attr_{};
    rknn_tensor_attr in_io_attr_{};
    rknn_tensor_mem *in_mem_ = nullptr;
    uint32_t in_copy_size_ = 0;
    bool zero_copy_input_enabled_ = false;
    int model_w_ = 640;
    int model_h_ = 640;
    int num_classes_ = 80;
    int io_num_in_ = 0;
    int io_num_out_ = 0;
    std::vector<rknn_tensor_attr> out_attrs_;
    std::vector<std::string> labels_;
    bool is_quant_ = true;

    float obj_thresh_ = 0.4f;
    float nms_thresh_ = 0.45f;

    /* 预缓存 RGA dst handle, 0=未初始化/失败 */
    int input_rga_handle_ = 0;
};
