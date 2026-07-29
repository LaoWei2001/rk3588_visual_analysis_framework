#pragma once
#include "model_base.h"
#include "rknn_api.h"
#include "yolo_utils.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

struct YoloPoseLetterBoxInfo
{
    float scale;
    float x_pad;
    float y_pad;
};

class YoloPose : public ModelBase
{
  public:
    YoloPose(const std::string &model_path, const std::string &label_path, int core_mask = RKNN_NPU_CORE_0_1_2,
             float obj_thresh = 0.5f, float nms_thresh = 0.45f);
    /* 保留旧构造接口，未提供标签时根据关键点数量使用 person/hand/pose。 */
    YoloPose(const std::string &model_path, int core_mask = RKNN_NPU_CORE_0_1_2, float obj_thresh = 0.5f,
             float nms_thresh = 0.45f);
    virtual ~YoloPose();

    virtual bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) override;

    /* ---- 零拷贝接口: 与 YOLO (v5) / YoloV8Det 完全对称 ---- */
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
    void query_model_info();
    void configure_pose_layout();
    void load_pose_label(const std::string &label_path);
    bool output_grid_size(const rknn_tensor_attr &attr, int &grid_h, int &grid_w) const;
    float keypoint_value(const float *buffer, int keypoint_index, int component, int candidate_index) const;
    bool init_zero_copy_input();
    cv::Mat preprocess(cv::Mat &img, YoloPoseLetterBoxInfo &lb);

    int process_i8(int8_t *input, int grid_h, int grid_w, int stride, std::vector<float> &boxes,
                   std::vector<float> &boxScores, std::vector<int> &classId, int32_t zp, float scale, int index);
    int process_fp32(float *input, int grid_h, int grid_w, int stride, std::vector<float> &boxes,
                     std::vector<float> &boxScores, std::vector<int> &classId, int32_t zp, float scale, int index);

    int post_process(rknn_output *outputs, YoloPoseLetterBoxInfo &lb, std::vector<AlgoResult> &results);
    void softmax(float *input, int size);
    float box_iou(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                  float ymax1);

  private:
    rknn_context ctx_ = 0;
    int io_num_in_ = 0;
    int io_num_out_ = 0;
    rknn_tensor_attr in_attr_{};
    rknn_tensor_attr in_io_attr_{};
    std::vector<rknn_tensor_attr> out_attrs_;

    int model_w_ = 640;
    int model_h_ = 640;
    int model_c_ = 3;
    bool is_quant_ = false;

    /* 当前已跑通的 RKNN Pose 布局为 3 个 DFL 检测头 + 1 个关键点输出。
     * 关键点输出支持 [K*3, N] / [K,3,N] 及
     * [N,K*3] / [N,K,3]。 */
    int detection_head_count_ = 3;
    int keypoint_output_index_ = -1;
    int total_grid_points_ = 0;
    int keypoint_count_ = 0;
    int keypoint_values_per_candidate_ = 0;
    bool keypoints_channels_first_ = true;
    std::string pose_label_ = "pose";

    float obj_thresh_ = 0.5f;
    float nms_thresh_ = 0.45f;

    /* ---- 零拷贝输入相关 ---- */
    rknn_tensor_mem *in_mem_ = nullptr;
    uint32_t in_copy_size_ = 0;
    bool zero_copy_input_enabled_ = false;

    /* ---- 推理输出缓存 (热路径复用) ---- */
    std::vector<rknn_output> rknn_outputs_cache_;

    /* 预缓存 RGA dst handle, 0=未初始化/失败 */
    int input_rga_handle_ = 0;
};
