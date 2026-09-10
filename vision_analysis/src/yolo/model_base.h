#pragma once

#include "inference/inference_types.h"
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

struct YoloPerfStat
{
    float preprocess_ms = 0.0f;
    float infer_ms = 0.0f;
    float postprocess_ms = 0.0f;
};

class ModelBase
{
  public:
    ModelBase()
    {
        pthread_mutex_init(&infer_mtx, nullptr);
    }
    virtual ~ModelBase()
    {
        pthread_mutex_destroy(&infer_mtx);
    }

    /* 推理互斥锁: 多通道共享同一模型实例时, 保证 RKNN ctx 串行访问.
     * 独占实例时不存在竞争, lock/unlock 开销可忽略不计 (~20ns).
     * 锁定范围: RGA 写入输入缓冲区 → NPU 推理完成 (需覆盖零拷贝写和 infer). */
    pthread_mutex_t infer_mtx;

    /**
     * @brief Perform inference on a frame
     * @param frame The input image
     * @param results Output list of detected objects/pose/text
     * @param perf Performance statistics output (optional)
     * @return true on success, false on failure
     */
    virtual bool infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr) = 0;

    /**
     * @brief 获取硬件物理显存句柄 (DMA-BUF)
     */
    virtual int get_input_fd() const
    {
        return -1;
    }

    /**
     * @brief 返回 NPU 输入 buffer 的预缓存 RGA handle (rga_buffer_handle_t 即 int).
     *        模型初始化时一次性 importbuffer_fd, 析构时 releasebuffer_handle.
     *        避免每帧都做 import/release 的 ioctl 开销 (每帧节省 ~2 次系统调用).
     *        返回 0 表示未缓存, 调用方应退回每帧 import 路径 (兜底安全).
     *        使用 int 类型以避免在 model_base.h 引入 <rga/im2d.h> 依赖.
     */
    virtual int get_input_rga_handle() const
    {
        return 0;
    }

    /**
     * @brief 零拷贝推理接口
     * 若派生类支持(如 YOLO)，则在 RGA 已写好内存后调用此接口跳过前处理。
     */
    virtual bool infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf = nullptr)
    {
        return false;
    }

    virtual int input_width() const = 0;
    virtual int input_height() const = 0;

    virtual void set_thresh(float obj_thresh, float nms_thresh) = 0;
    virtual float get_obj_thresh() const = 0;

    /**
     * @brief 绑定配置中的模型身份，并统一写入该模型产生的每条结果。
     *
     * 单模型和组合模型必须遵守同一来源元数据契约；具体后处理器不需要知道配置节点 ID。
     */
    void set_result_source(std::string model_id, std::string model_type, int model_index)
    {
        result_model_id_ = std::move(model_id);
        result_model_type_ = std::move(model_type);
        result_model_index_ = model_index;
    }

    void annotate_results(std::vector<AlgoResult> &results) const
    {
        if (result_model_type_.empty())
            return;
        for (AlgoResult &result : results)
        {
            result.model_id = result_model_id_;
            result.model_type = result_model_type_;
            result.model_index = result_model_index_;
        }
    }

    /**
     * @brief Whether the model already performs NMS internally during post_process.
     *        If true, the pipeline will skip the redundant nms_inplace() call.
     */
    virtual bool nms_done() const
    {
        return false;
    }

  private:
    std::string result_model_id_;
    std::string result_model_type_;
    int result_model_index_ = 0;
};
