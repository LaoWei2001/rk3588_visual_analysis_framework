#include "yolov8det.h"
#include "detection_order.h"
#include "rknn_query_utils.h"
#include "vision/inference/npu_execution.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <rga/im2d.h>
#include <set>

YoloV8Det::YoloV8Det(const std::string &model_path, const std::string &label_path, int core_mask, float obj_thresh,
                     float nms_thresh, bool obb)
{
    obb_ = obb;
    obj_thresh_ = obj_thresh;
    nms_thresh_ = nms_thresh;
    if (!obb_ || !label_path.empty())
        load_labels(label_path);
    try
    {
        init_rknn(model_path, core_mask);
        query_model_info();
        init_zero_copy_input();
        outputs_.initialize(ctx_, out_attrs_, !is_quant_, zero_copy_input_enabled_);
        printf("[YoloV8Det] native outputs=%d\n", outputs_.native());
    }
    catch (...)
    {
        if (input_rga_handle_)
            releasebuffer_handle(static_cast<rga_buffer_handle_t>(input_rga_handle_));
        if (in_mem_ && ctx_)
            rknn_destroy_mem(ctx_, in_mem_);
        outputs_.reset();
        if (ctx_)
            rknn_destroy(ctx_);
        throw;
    }
}

YoloV8Det::~YoloV8Det()
{
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
    outputs_.reset();
    if (ctx_ > 0)
        rknn_destroy(ctx_);
}

void YoloV8Det::load_labels(const std::string &path)
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

void YoloV8Det::init_rknn(const std::string &model_path, int core_mask)
{
    int ret = rknn_init(&ctx_, (void *)model_path.c_str(), 0, 0, NULL);
    if (ret < 0)
    {
        throw std::runtime_error("RKNN init failed");
    }
    ret = rknn_set_core_mask(ctx_, (rknn_core_mask)core_mask);
    npu_core_mask_ = ret < 0 ? 0 : core_mask;
    if (ret < 0)
    {
        printf("[YoloV8Det] warn: set core mask failed, fallback single core\n");
    }
    else
    {
        printf("[YoloV8Det] use core mask 0x%x\n", core_mask);
    }
}

void YoloV8Det::query_model_info()
{
    if (obb_)
    {
        query_obb_info();
        return;
    }
    rknn_input_output_num io_num{};
    model_io::query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    model_io::validate_io(io_num);
    if (io_num.n_output != 9)
        throw std::runtime_error("Unsupported yolov8det output count");
    io_num_in_ = io_num.n_input;
    io_num_out_ = io_num.n_output;

    memset(&in_attr_, 0, sizeof(in_attr_));
    in_attr_.index = 0;
    model_io::query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
    model_io::validate_image(in_attr_);

    if (in_attr_.fmt == RKNN_TENSOR_NCHW)
    {
        model_h_ = in_attr_.dims[2];
        model_w_ = in_attr_.dims[3];
    }
    else
    {
        model_h_ = in_attr_.dims[1];
        model_w_ = in_attr_.dims[2];
    }

    out_attrs_.clear();
    for (int i = 0; i < io_num_out_; i++)
    {
        rknn_tensor_attr attr{};
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        model_io::query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (attr.n_dims != 4 || !attr.n_elems)
            throw std::runtime_error("Invalid detection output tensor");
        out_attrs_.push_back(attr);
    }

    is_quant_ = model_io::all_int8(out_attrs_);
    if (io_num_out_ > 1)
    {
        const rknn_tensor_attr &score_attr = out_attrs_[1];
        if (score_attr.fmt == RKNN_TENSOR_NHWC)
        {
            num_classes_ = score_attr.dims[3];
        }
        else
        {
            num_classes_ = score_attr.dims[1];
        }
    }
    if (num_classes_ <= 0)
    {
        num_classes_ = labels_.empty() ? 80 : (int)labels_.size();
    }

    const unsigned dfl_channels = out_attrs_[0].dims[1];
    if (dfl_channels < 4 || dfl_channels % 4)
        throw std::runtime_error("Invalid DFL channels");
    for (int i = 0; i < 3; ++i)
    {
        model_io::validate_head(out_attrs_[3 * i], model_w_, model_h_, dfl_channels, 8 << i);
        model_io::validate_head(out_attrs_[3 * i + 1], model_w_, model_h_, num_classes_, 8 << i);
    }
    printf("[YoloV8Det] model %dx%d, classes=%d, quant=%d, outputs=%d\n", model_w_, model_h_, num_classes_, is_quant_,
           io_num_out_);

    /* 预分配热路径缓存 */
}

bool YoloV8Det::init_zero_copy_input()
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
        if (in_mem_)
            rknn_destroy_mem(ctx_, in_mem_);
        in_mem_ = nullptr;
        printf("[YoloV8Det] zero-copy input disabled: rknn_create_mem failed\n");
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
        printf("[YoloV8Det] zero-copy input disabled: rknn_set_io_mem failed(%d)\n", ret);
        rknn_destroy_mem(ctx_, in_mem_);
        in_mem_ = nullptr;
        return false;
    }

    zero_copy_input_enabled_ = true;
    printf("[YoloV8Det] zero-copy input enabled, mem=%u bytes, fd=%d\n", alloc_size, in_mem_->fd);

    im_handle_param_t rga_dst_param{};
    rga_dst_param.width = static_cast<uint32_t>(model_w_);
    rga_dst_param.height = static_cast<uint32_t>(model_h_);
    rga_dst_param.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(in_mem_->fd, &rga_dst_param));
    if (input_rga_handle_ == 0)
        printf("[YoloV8Det] Warning: RGA dst handle cache failed, will import per-frame\n");
    else
        printf("[YoloV8Det] RGA dst handle cached (handle=%d)\n", input_rga_handle_);

    return true;
}

cv::Mat YoloV8Det::preprocess(cv::Mat &img, LetterBoxInfo &lb)
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
    cv::Mat canvas(model_h_, model_w_, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(lb.dw, lb.dh, nw, nh)));
    cv::cvtColor(canvas, canvas, cv::COLOR_BGR2RGB);
    return canvas;
}

void YoloV8Det::compute_dfl(float *tensor, int dfl_len, float *box)
{
    for (int b = 0; b < 4; b++)
    {
        float exp_t[dfl_len];
        float exp_sum = 0;
        float acc_sum = 0;
        for (int i = 0; i < dfl_len; i++)
        {
            exp_t[i] = expf(tensor[i + b * dfl_len]);
            exp_sum += exp_t[i];
        }
        for (int i = 0; i < dfl_len; i++)
        {
            acc_sum += exp_t[i] / exp_sum * i;
        }
        box[b] = acc_sum;
    }
}

int YoloV8Det::process_i8(int8_t *box_tensor, int32_t box_zp, float box_scale, int8_t *score_tensor, int32_t score_zp,
                          float score_scale, int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                          int grid_h, int grid_w, int stride, int dfl_len, int class_num, std::vector<float> &boxes,
                          std::vector<float> &objProbs, std::vector<int> &classId, float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;

    // [Cache-Friendly Optimization]
    // Sequential memory access across NCHW memory layout to avoid severe L1/L2 Cache-Thrashing
    std::vector<int8_t> max_scores(grid_len, -128);
    std::vector<int> max_class_ids(grid_len, -1);

    for (int c = 0; c < class_num; c++)
    {
        const int8_t *class_scores = score_tensor + c * grid_len;
        for (int k = 0; k < grid_len; k++)
        {
            // Compare in score space: a threshold above this tensor's INT8 range must
            // reject every candidate, rather than overflow when narrowed to int8_t.
            const float score = (static_cast<float>(class_scores[k]) - static_cast<float>(score_zp)) * score_scale;
            if (score >= threshold && (max_class_ids[k] < 0 || class_scores[k] > max_scores[k]))
            {
                max_scores[k] = class_scores[k];
                max_class_ids[k] = c;
            }
        }
    }

    // Process only valid grids sequentially
    for (int k = 0; k < grid_len; k++)
    {
        if (max_class_ids[k] != -1)
        {
            int i = k / grid_w;
            int j = k % grid_w;

            float box[4];
            /* thread_local 预分配, 避免每检测一次堆分配 */
            static thread_local std::vector<float> before_dfl;
            if (before_dfl.size() < static_cast<size_t>(dfl_len * 4))
                before_dfl.resize(dfl_len * 4);
            int box_offset = k;
            for (int k_dfl = 0; k_dfl < dfl_len * 4; k_dfl++)
            {
                before_dfl[k_dfl] = ((float)box_tensor[box_offset] - (float)box_zp) * box_scale;
                box_offset += grid_len;
            }
            compute_dfl(before_dfl.data(), dfl_len, box);

            float x1 = (-box[0] + j + 0.5f) * stride;
            float y1 = (-box[1] + i + 0.5f) * stride;
            float x2 = (box[2] + j + 0.5f) * stride;
            float y2 = (box[3] + i + 0.5f) * stride;

            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(x2 - x1);
            boxes.push_back(y2 - y1);
            objProbs.push_back(((float)max_scores[k] - (float)score_zp) * score_scale);
            classId.push_back(max_class_ids[k]);
            validCount++;
        }
    }
    return validCount;
}

int YoloV8Det::process_fp32(float *box_tensor, float *score_tensor, float *score_sum_tensor, int grid_h, int grid_w,
                            int stride, int dfl_len, int class_num, std::vector<float> &boxes,
                            std::vector<float> &objProbs, std::vector<int> &classId, float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;

    // [Cache-Friendly Optimization]
    // Sequential memory access across NCHW memory layout to avoid severe L1/L2 Cache-Thrashing
    std::vector<float> max_scores(grid_len, -1.0f);
    std::vector<int> max_class_ids(grid_len, -1);

    for (int c = 0; c < class_num; c++)
    {
        const float *class_scores = score_tensor + c * grid_len;
        for (int k = 0; k < grid_len; k++)
        {
            if (class_scores[k] > threshold && class_scores[k] > max_scores[k])
            {
                max_scores[k] = class_scores[k];
                max_class_ids[k] = c;
            }
        }
    }

    // Process only valid grids sequentially
    for (int k = 0; k < grid_len; k++)
    {
        if (max_class_ids[k] != -1)
        {
            int i = k / grid_w;
            int j = k % grid_w;

            float box[4];
            /* thread_local 预分配, 避免每检测一次堆分配 */
            static thread_local std::vector<float> before_dfl;
            if (before_dfl.size() < static_cast<size_t>(dfl_len * 4))
                before_dfl.resize(dfl_len * 4);
            int box_offset = k;
            for (int k_dfl = 0; k_dfl < dfl_len * 4; k_dfl++)
            {
                before_dfl[k_dfl] = box_tensor[box_offset];
                box_offset += grid_len;
            }
            compute_dfl(before_dfl.data(), dfl_len, box);

            float x1 = (-box[0] + j + 0.5f) * stride;
            float y1 = (-box[1] + i + 0.5f) * stride;
            float x2 = (box[2] + j + 0.5f) * stride;
            float y2 = (box[3] + i + 0.5f) * stride;

            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(x2 - x1);
            boxes.push_back(y2 - y1);
            objProbs.push_back(max_scores[k]);
            classId.push_back(max_class_ids[k]);
            validCount++;
        }
    }
    return validCount;
}
// calculate_overlap 使用 yolo_utils.h 中的 compute_iou(6-params)
using ::compute_iou;

int YoloV8Det::nms(int validCount, std::vector<float> &outputLocations, std::vector<int> &classIds,
                   std::vector<int> &order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
            continue;

        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
                continue;

            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = compute_iou(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > threshold)
                order[j] = -1;
        }
    }
    return 0;
}

int YoloV8Det::post_process(rknn_output *outputs, LetterBoxInfo &lb, std::vector<AlgoResult> &results)
{
    if (obb_)
        return post_process_obb(outputs, lb, results);
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;
    /* 预分配避免热路径多次扩容: 最大检测数 ~8400 (80²+40²+20²) */
    filterBoxes.reserve(4096);
    objProbs.reserve(1024);
    classId.reserve(1024);

    int stride_arr[] = {8, 16, 32};
    int dfl_len = out_attrs_[0].dims[1] / 4; // 动态计算DFL长度
    int validCount = 0;

    if (num_classes_ <= 0)
    {
        return 0;
    }

    for (int i = 0; i < 3; i++)
    {
        int grid_h = model_h_ / stride_arr[i];
        int grid_w = model_w_ / stride_arr[i];

        if (is_quant_)
        {
            int8_t *box_tensor = (int8_t *)outputs[i * 3].buf;
            int8_t *score_tensor = (int8_t *)outputs[i * 3 + 1].buf;
            int8_t *score_sum_tensor = nullptr;

            int32_t box_zp = out_attrs_[i * 3].zp;
            int32_t score_zp = out_attrs_[i * 3 + 1].zp;
            float box_scale = out_attrs_[i * 3].scale;
            float score_scale = out_attrs_[i * 3 + 1].scale;

            validCount += process_i8(box_tensor, box_zp, box_scale, score_tensor, score_zp, score_scale,
                                     score_sum_tensor, 0, 1.0f, grid_h, grid_w, stride_arr[i], dfl_len, num_classes_,
                                     filterBoxes, objProbs, classId, obj_thresh_);
        }
        else
        {
            float *box_tensor = (float *)outputs[i * 3].buf;
            float *score_tensor = (float *)outputs[i * 3 + 1].buf;
            float *score_sum_tensor = nullptr;

            validCount += process_fp32(box_tensor, score_tensor, score_sum_tensor, grid_h, grid_w, stride_arr[i],
                                       dfl_len, num_classes_, filterBoxes, objProbs, classId, obj_thresh_);
        }
    }

    if (validCount <= 0)
        return 0;

    std::vector<int> indexArray(validCount);
    for (int i = 0; i < validCount; i++)
        indexArray[i] = i;
    sort_detection_indices(objProbs, indexArray);

    std::set<int> class_set;
    for (int i = 0; i < validCount; i++)
    {
        if (indexArray[i] != -1)
            class_set.insert(classId[indexArray[i]]);
    }

    for (auto c : class_set)
    {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_thresh_);
    }

    for (int i = 0; i < validCount; i++)
    {
        if (indexArray[i] == -1)
            continue;
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float w = filterBoxes[n * 4 + 2];
        float h = filterBoxes[n * 4 + 3];

        x1 = (x1 - lb.dw) / lb.ratio;
        y1 = (y1 - lb.dh) / lb.ratio;
        w = w / lb.ratio;
        h = h / lb.ratio;

        AlgoResult res;
        res.box = cv::Rect((int)x1, (int)y1, (int)w, (int)h);
        res.score = objProbs[n];
        res.class_id = classId[n];
        res.label = (classId[n] < (int)labels_.size()) ? labels_[classId[n]] : std::to_string(classId[n]);
        results.push_back(res);
    }

    return results.size();
}

bool YoloV8Det::infer(cv::Mat &frame, std::vector<AlgoResult> &results, ModelPerfStat *perf)
{
    results.clear();
    if (frame.empty())
        return false;
    const auto t0 = std::chrono::steady_clock::now();
    LetterBoxInfo lb;
    cv::Mat input_mat = preprocess(frame, lb);
    rknn_input input{};
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = model_w_ * model_h_ * 3;
    input.buf = input_mat.data;
    if (zero_copy_input_enabled_)
    {
        // Keep DMA bindings intact when a frame falls back to CPU preprocessing.
        if (!in_mem_ || !in_mem_->virt_addr || in_copy_size_ > in_mem_->size)
            return false;
        std::memcpy(in_mem_->virt_addr, input_mat.data, in_copy_size_);
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t2;
    const int ret = outputs_.execute(npu_core_mask_, zero_copy_input_enabled_ ? nullptr : &input, 1, t2);
    if (ret < 0)
    {
        printf("[YoloV8Det] RKNN execution failed: %d\n", ret);
        return false;
    }
    post_process(outputs_.data(), lb, results);
    const auto t3 = std::chrono::steady_clock::now();
    if (perf)
    {
        perf->preprocess_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}

bool YoloV8Det::infer_zero_copy(std::vector<AlgoResult> &results, ModelPerfStat *perf)
{
    results.clear();
    if (!zero_copy_input_enabled_ || !in_mem_ || in_mem_->fd < 0)
        return false;
    const auto t1 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t2;
    const int ret = outputs_.execute(npu_core_mask_, nullptr, 0, t2);
    if (ret < 0)
    {
        printf("[YoloV8Det] RKNN DMA execution failed: %d\n", ret);
        return false;
    }
    // RGA stretches to the model canvas; pipeline maps results to the source.
    LetterBoxInfo lb;
    lb.ratio = 1.f;
    lb.dw = 0;
    lb.dh = 0;
    post_process(outputs_.data(), lb, results);
    const auto t3 = std::chrono::steady_clock::now();
    if (perf)
    {
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}

#include "yolov8obb_impl.h"
