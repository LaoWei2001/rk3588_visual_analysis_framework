#include "yolo.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <rga/im2d.h>

static const float ANCHORS[3][3][2] = {
    {{10, 13}, {16, 30}, {33, 23}}, {{30, 61}, {62, 45}, {59, 119}}, {{116, 90}, {156, 198}, {373, 326}}};

YOLO::YOLO(const std::string &model_path, const std::string &label_path, int core_mask, float obj_thresh,
           float nms_thresh)
{
    obj_thresh_ = obj_thresh;
    nms_thresh_ = nms_thresh;
    load_labels(label_path);
    init_rknn(model_path, core_mask);
    query_model_info();
    init_zero_copy_input();
}

YOLO::~YOLO()
{
    /* 必须在 rknn_destroy_mem 之前释放 RGA handle:
     * releasebuffer_handle 通知 librga 减少 dma_buf 引用计数,
     * 之后 rknn_destroy_mem 才能安全释放物理内存. */
    if (input_rga_handle_ != 0)
    {
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(input_rga_handle_));
        input_rga_handle_ = 0;
    }
    if (in_mem_)
    {
        rknn_destroy_mem(ctx_, in_mem_);
        in_mem_ = nullptr;
    }
    if (ctx_ > 0)
        rknn_destroy(ctx_);
}

void YOLO::load_labels(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("cannot open labels: " + path);
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            labels_.push_back(line);
    }
    file.close();
}

void YOLO::init_rknn(const std::string &model_path, int core_mask)
{
    int ret = rknn_init(&ctx_, (void *)model_path.c_str(), 0, 0, NULL);
    if (ret < 0)
    {
        throw std::runtime_error("RKNN init failed");
    }
    ret = rknn_set_core_mask(ctx_, (rknn_core_mask)core_mask);
    if (ret < 0)
    {
        printf("[YOLO] warn: set core mask failed, fallback single core\n");
    }
    else
    {
        printf("[YOLO] use core mask 0x%x\n", core_mask);
    }
}

void YOLO::query_model_info()
{
    rknn_input_output_num io_num;
    rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    io_num_in_ = io_num.n_input;
    io_num_out_ = io_num.n_output;

    memset(&in_attr_, 0, sizeof(in_attr_));
    in_attr_.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
    model_w_ = in_attr_.dims[2];
    model_h_ = in_attr_.dims[1];

    out_attrs_.clear();
    for (int i = 0; i < io_num_out_; i++)
    {
        rknn_tensor_attr attr;
        attr.index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        out_attrs_.push_back(attr);
    }
    num_classes_ = (out_attrs_[0].dims[1] / 3) - 5;

    is_quant_ = false;
    if (out_attrs_[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && out_attrs_[0].type != RKNN_TENSOR_FLOAT16)
    {
        is_quant_ = true;
    }

    printf("[YOLO] model %dx%d, classes %d, is_quant=%d\n", model_w_, model_h_, num_classes_, is_quant_);

    /* 提前分配热路径缓存容量 */
    rknn_outputs_cache_.resize(io_num_out_);
    candidates_cache_.reserve(256);
}

bool YOLO::init_zero_copy_input()
{
    zero_copy_input_enabled_ = false;
    in_copy_size_ = static_cast<uint32_t>(model_w_ * model_h_ * 3);

    uint32_t alloc_size = in_copy_size_;
    if (in_attr_.size_with_stride > alloc_size)
    {
        alloc_size = in_attr_.size_with_stride;
    }

    in_mem_ = rknn_create_mem(ctx_, alloc_size);
    if (!in_mem_ || !in_mem_->virt_addr)
    {
        in_mem_ = nullptr;
        printf("[YOLO] zero-copy input disabled: rknn_create_mem failed\n");
        return false;
    }

    in_io_attr_ = in_attr_;
    in_io_attr_.index = 0;
    in_io_attr_.type = RKNN_TENSOR_UINT8;
    in_io_attr_.fmt = RKNN_TENSOR_NHWC;
    in_io_attr_.pass_through = 0;
    in_io_attr_.h_stride = 0;

    int ret = rknn_set_io_mem(ctx_, in_mem_, &in_io_attr_);
    if (ret < 0)
    {
        printf("[YOLO] zero-copy input disabled: rknn_set_io_mem failed(%d)\n", ret);
        rknn_destroy_mem(ctx_, in_mem_);
        in_mem_ = nullptr;
        return false;
    }

    zero_copy_input_enabled_ = true;
    printf("[YOLO] zero-copy input enabled, mem=%u bytes\n", alloc_size);

    /* 一次性缓存 RGA dst handle: 避免每帧 importbuffer_fd/releasebuffer_handle
     * 的 ioctl 开销. 参数与 worker 调用 rga_convert_resize_handle 时一致:
     * stride_w = model_w_, stride_h = model_h_, format = RK_FORMAT_RGB_888. */
    im_handle_param_t rga_dst_param{};
    rga_dst_param.width = static_cast<uint32_t>(model_w_);
    rga_dst_param.height = static_cast<uint32_t>(model_h_);
    rga_dst_param.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(in_mem_->fd, &rga_dst_param));
    if (input_rga_handle_ == 0)
        printf("[YOLO] Warning: RGA dst handle cache failed, will import per-frame\n");
    else
        printf("[YOLO] RGA dst handle cached (handle=%d)\n", input_rga_handle_);

    return true;
}

cv::Mat YOLO::preprocess(cv::Mat &img, LetterBoxInfo &lb)
{
    if (img.cols == model_w_ && img.rows == model_h_)
    {
        lb.ratio = 1.0f;
        lb.dw = 0;
        lb.dh = 0;
        cv::Mat rgb;
        cv::cvtColor(img, rgb, cv::COLOR_BGR2RGB);
        return rgb;
    }

    float r = std::min((float)model_w_ / img.cols, (float)model_h_ / img.rows);
    int nw = (int)(img.cols * r), nh = (int)(img.rows * r);
    lb.ratio = r;
    lb.dw = (model_w_ - nw) / 2;
    lb.dh = (model_h_ - nh) / 2;

    cv::Mat resized;
    cv::resize(img, resized, cv::Size(nw, nh));
    cv::Mat canvas = cv::Mat(model_h_, model_w_, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(lb.dw, lb.dh, nw, nh)));
    cv::cvtColor(canvas, canvas, cv::COLOR_BGR2RGB);
    return canvas;
}

void YOLO::post_process(void *feat, int out_idx, int feat_h, int feat_w, LetterBoxInfo &lb,
                        std::vector<AlgoResult> &results)
{
    int output_start = 0;
    if (io_num_out_ > 3)
        output_start = io_num_out_ - 3;
    if (out_idx < output_start)
        return;
    int a_idx = out_idx - output_start;

    int grid_h = feat_h;
    int grid_w = feat_w;
    int stride = model_h_ / grid_h;
    int prop_box = 5 + num_classes_;

    bool anchor_last_layout = false;
    if (out_attrs_[out_idx].n_dims >= 5)
    {
        int last_dim = out_attrs_[out_idx].dims[out_attrs_[out_idx].n_dims - 1];
        if (out_attrs_[out_idx].dims[1] == 3 && last_dim == prop_box)
            anchor_last_layout = true;
    }

    int grid_len = grid_h * grid_w;
    float threshold = obj_thresh_;
    int32_t zp = out_attrs_[out_idx].zp;
    float scale = out_attrs_[out_idx].scale;

    // 量化工具函数使用 yolo_utils.h 中的全局 inline 版本
    using ::deqnt_affine_to_f32;
    using ::qnt_f32_to_affine;

    int8_t thres_i8 = is_quant_ ? qnt_f32_to_affine(threshold, zp, scale) : 0;

    for (int a = 0; a < 3; a++)
    {
        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                float box_x, box_y, box_w, box_h, box_confidence, maxClassProbs;
                int maxClassId = 0;

                if (is_quant_)
                {
                    int8_t *in_ptr;
                    if (anchor_last_layout)
                    {
                        int offset = ((a * grid_h + i) * grid_w + j) * prop_box;
                        in_ptr = (int8_t *)feat + offset;
                    }
                    else
                    {
                        int offset = (prop_box * a) * grid_len + i * grid_w + j;
                        in_ptr = (int8_t *)feat + offset;
                    }

                    int8_t box_conf_i8 = anchor_last_layout ? in_ptr[4] : in_ptr[4 * grid_len];
                    if (box_conf_i8 < thres_i8)
                        continue;

                    int8_t max_prob_i8 = anchor_last_layout ? in_ptr[5] : in_ptr[5 * grid_len];
                    for (int k = 1; k < num_classes_; ++k)
                    {
                        int8_t prob = anchor_last_layout ? in_ptr[5 + k] : in_ptr[(5 + k) * grid_len];
                        if (prob > max_prob_i8)
                        {
                            maxClassId = k;
                            max_prob_i8 = prob;
                        }
                    }
                    if (max_prob_i8 <= thres_i8)
                        continue;

                    box_confidence = deqnt_affine_to_f32(box_conf_i8, zp, scale);
                    maxClassProbs = deqnt_affine_to_f32(max_prob_i8, zp, scale);

                    if (anchor_last_layout)
                    {
                        box_x = deqnt_affine_to_f32(in_ptr[0], zp, scale) * 2.0f - 0.5f;
                        box_y = deqnt_affine_to_f32(in_ptr[1], zp, scale) * 2.0f - 0.5f;
                        box_w = deqnt_affine_to_f32(in_ptr[2], zp, scale) * 2.0f;
                        box_h = deqnt_affine_to_f32(in_ptr[3], zp, scale) * 2.0f;
                    }
                    else
                    {
                        box_x = deqnt_affine_to_f32(in_ptr[0], zp, scale) * 2.0f - 0.5f;
                        box_y = deqnt_affine_to_f32(in_ptr[grid_len], zp, scale) * 2.0f - 0.5f;
                        box_w = deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale) * 2.0f;
                        box_h = deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale) * 2.0f;
                    }
                }
                else
                {
                    float *in_ptr;
                    if (anchor_last_layout)
                    {
                        int offset = ((a * grid_h + i) * grid_w + j) * prop_box;
                        in_ptr = (float *)feat + offset;
                    }
                    else
                    {
                        int offset = (prop_box * a) * grid_len + i * grid_w + j;
                        in_ptr = (float *)feat + offset;
                    }

                    box_confidence = anchor_last_layout ? in_ptr[4] : in_ptr[4 * grid_len];
                    if (box_confidence < threshold)
                        continue;

                    maxClassProbs = anchor_last_layout ? in_ptr[5] : in_ptr[5 * grid_len];
                    for (int k = 1; k < num_classes_; ++k)
                    {
                        float prob = anchor_last_layout ? in_ptr[5 + k] : in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs)
                        {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }
                    if (maxClassProbs <= threshold)
                        continue;

                    if (anchor_last_layout)
                    {
                        box_x = in_ptr[0] * 2.0f - 0.5f;
                        box_y = in_ptr[1] * 2.0f - 0.5f;
                        box_w = in_ptr[2] * 2.0f;
                        box_h = in_ptr[3] * 2.0f;
                    }
                    else
                    {
                        box_x = in_ptr[0] * 2.0f - 0.5f;
                        box_y = in_ptr[grid_len] * 2.0f - 0.5f;
                        box_w = in_ptr[2 * grid_len] * 2.0f;
                        box_h = in_ptr[3 * grid_len] * 2.0f;
                    }
                }

                float cx = (box_x + j) * (float)stride;
                float cy = (box_y + i) * (float)stride;
                float rw = box_w * box_w * ANCHORS[a_idx][a][0];
                float rh = box_h * box_h * ANCHORS[a_idx][a][1];

                AlgoResult res;
                res.box.x = (int)((cx - rw / 2.0f - lb.dw) / lb.ratio);
                res.box.y = (int)((cy - rh / 2.0f - lb.dh) / lb.ratio);
                res.box.width = (int)(rw / lb.ratio);
                res.box.height = (int)(rh / lb.ratio);
                res.score = maxClassProbs * box_confidence;
                res.class_id = maxClassId;
                res.label = (maxClassId < (int)labels_.size()) ? labels_[maxClassId] : std::to_string(maxClassId);
                results.push_back(res);
            }
        }
    }
}

bool YOLO::infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (frame.empty())
        return false;

    auto t0 = std::chrono::steady_clock::now();
    LetterBoxInfo lb;
    cv::Mat input_mat = preprocess(frame, lb);
    auto t1 = std::chrono::steady_clock::now();

    if (zero_copy_input_enabled_ && in_mem_ && in_mem_->virt_addr)
    {
        if (in_copy_size_ > in_mem_->size)
        {
            return false;
        }
        memcpy(in_mem_->virt_addr, input_mat.data, in_copy_size_);
    }
    else
    {
        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].size = model_w_ * model_h_ * 3;
        inputs[0].buf = input_mat.data;
        rknn_inputs_set(ctx_, 1, inputs);
    }

    if (rknn_run(ctx_, NULL) < 0)
        return false;
    auto t2 = std::chrono::steady_clock::now();

    /* 使用预分配的缓存进行输出读取 */
    for (int i = 0; i < io_num_out_; i++)
    {
        memset(&rknn_outputs_cache_[i], 0, sizeof(rknn_output));
        rknn_outputs_cache_[i].want_float = (!is_quant_);
    }
    if (rknn_outputs_get(ctx_, io_num_out_, rknn_outputs_cache_.data(), NULL) < 0)
        return false;

    /* 复用内存存储检测结果 */
    candidates_cache_.clear();
    for (int i = 0; i < io_num_out_; i++)
    {
        post_process(rknn_outputs_cache_[i].buf, i, out_attrs_[i].dims[2], out_attrs_[i].dims[3], lb,
                     candidates_cache_);
    }
    auto t3 = std::chrono::steady_clock::now();

    /* 以极低开销将结果转移给调用方 */
    results.swap(candidates_cache_);

    rknn_outputs_release(ctx_, io_num_out_, rknn_outputs_cache_.data());

    if (perf)
    {
        perf->preprocess_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}

bool YOLO::infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (!zero_copy_input_enabled_ || !in_mem_ || in_mem_->fd < 0)
        return false;

    /* RGA FD→FD 路径已直接输出 RGB888 到 NPU 显存, 无需 CPU 通道交换 */

    auto t1 = std::chrono::steady_clock::now();
    /* NPU 已被 RGA 在硬件层填好数据，直接执行推理 */
    if (rknn_run(ctx_, NULL) < 0)
        return false;
    auto t2 = std::chrono::steady_clock::now();

    for (int i = 0; i < io_num_out_; i++)
    {
        memset(&rknn_outputs_cache_[i], 0, sizeof(rknn_output));
        rknn_outputs_cache_[i].want_float = (!is_quant_);
    }
    if (rknn_outputs_get(ctx_, io_num_out_, rknn_outputs_cache_.data(), NULL) < 0)
        return false;

    candidates_cache_.clear();
    LetterBoxInfo lb;
    /* 零拷贝时，RGA 会将画面等比缩放并居中(或者填满)，我们需要告诉后处理当时的
     * padding。 目前 RGA 是直接 stretch 到 640x640，所以 ratio=1.0, dw=0, dh=0.
     * 如果 RGA 做了 letterbox，需要把这些参数作为参数传入 infer_zero_copy。
     * 这里为了简化且兼容现有 RGA stretch 的行为，直接使用全屏 ratio. */
    lb.ratio = 1.0f;
    lb.dw = 0;
    lb.dh = 0;

    for (int i = 0; i < io_num_out_; i++)
    {
        post_process(rknn_outputs_cache_[i].buf, i, out_attrs_[i].dims[2], out_attrs_[i].dims[3], lb,
                     candidates_cache_);
    }
    auto t3 = std::chrono::steady_clock::now();

    results.swap(candidates_cache_);
    rknn_outputs_release(ctx_, io_num_out_, rknn_outputs_cache_.data());

    if (perf)
    {
        /* preprocess_ms 已由调用方统计 RGA 耗时; 此处仅填充 NPU + 后处理 */
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}