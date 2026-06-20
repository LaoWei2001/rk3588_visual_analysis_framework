#include "yolopose.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <rga/im2d.h>

YoloPose::YoloPose(const std::string &model_path, int core_mask,
                   float obj_thresh, float nms_thresh)
{
    obj_thresh_ = obj_thresh;
    nms_thresh_ = nms_thresh;
    init_rknn(model_path, core_mask);
    query_model_info();
    init_zero_copy_input();
}

YoloPose::~YoloPose()
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
    if (ctx_ > 0)
        rknn_destroy(ctx_);
}

void YoloPose::init_rknn(const std::string &model_path, int core_mask)
{
    int ret = rknn_init(&ctx_, (void *)model_path.c_str(), 0, 0, NULL);
    if (ret < 0)
    {
        throw std::runtime_error("RKNN init failed for YoloPose");
    }
    ret = rknn_set_core_mask(ctx_, (rknn_core_mask)core_mask);
    if (ret < 0)
    {
        printf("[YoloPose] warn: set core mask failed, fallback single core\n");
    }
    else
    {
        printf("[YoloPose] use core mask 0x%x\n", core_mask);
    }
}

void YoloPose::query_model_info()
{
    rknn_input_output_num io_num;
    rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    io_num_in_ = io_num.n_input;
    io_num_out_ = io_num.n_output;

    memset(&in_attr_, 0, sizeof(in_attr_));
    in_attr_.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));

    if (in_attr_.fmt == RKNN_TENSOR_NCHW)
    {
        model_c_ = in_attr_.dims[1];
        model_h_ = in_attr_.dims[2];
        model_w_ = in_attr_.dims[3];
    }
    else
    {
        model_h_ = in_attr_.dims[1];
        model_w_ = in_attr_.dims[2];
        model_c_ = in_attr_.dims[3];
    }

    out_attrs_.clear();
    for (int i = 0; i < io_num_out_; i++)
    {
        rknn_tensor_attr attr;
        attr.index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        out_attrs_.push_back(attr);
    }
    // printf("[YoloPose] model %dx%d, inputs %d, outputs %d\n", model_w_, model_h_, io_num_in_, io_num_out_);

    /* 预分配热路径缓存 */
    rknn_outputs_cache_.resize(io_num_out_);
}

bool YoloPose::init_zero_copy_input()
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
        printf("[YoloPose] zero-copy input disabled: rknn_create_mem failed\n");
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
        printf("[YoloPose] zero-copy input disabled: rknn_set_io_mem failed(%d)\n", ret);
        rknn_destroy_mem(ctx_, in_mem_);
        in_mem_ = nullptr;
        return false;
    }

    zero_copy_input_enabled_ = true;
    printf("[YoloPose] zero-copy input enabled, mem=%u bytes, fd=%d\n", alloc_size, in_mem_->fd);

    im_handle_param_t rga_dst_param{};
    rga_dst_param.width  = static_cast<uint32_t>(model_w_);
    rga_dst_param.height = static_cast<uint32_t>(model_h_);
    rga_dst_param.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(in_mem_->fd, &rga_dst_param));
    if (input_rga_handle_ == 0)
        printf("[YoloPose] Warning: RGA dst handle cache failed, will import per-frame\n");
    else
        printf("[YoloPose] RGA dst handle cached (handle=%d)\n", input_rga_handle_);

    return true;
}

cv::Mat YoloPose::preprocess(cv::Mat &img, YoloPoseLetterBoxInfo &lb)
{
    int img_w = img.cols;
    int img_h = img.rows;
    float scale = std::min((float)model_w_ / img_w, (float)model_h_ / img_h);
    int new_w = std::round(img_w * scale);
    int new_h = std::round(img_h * scale);
    int pad_w = model_w_ - new_w;
    int pad_h = model_h_ - new_h;

    lb.scale = scale;
    lb.x_pad = pad_w / 2.0f;
    lb.y_pad = pad_h / 2.0f;

    cv::Mat resized;
    cv::resize(img, resized, cv::Size(new_w, new_h));

    cv::Mat canvas(model_h_, model_w_, CV_8UC3, cv::Scalar(114, 114, 114));
    // The top-left corner is where we copy to. We must use integer conversion.
    int top = std::round(lb.y_pad);
    int left = std::round(lb.x_pad);
    // Be very careful that left and top correspond exactly to the substracted padding!
    // If integer truncation makes them off by 0.5 pixels it's fine, but lb.x_pad should be exact

    // In yolo logic, it uses: lb.dw = (model_w_ - nw) / 2;
    lb.x_pad = (model_w_ - new_w) / 2;
    lb.y_pad = (model_h_ - new_h) / 2;
    left = lb.x_pad;
    top = lb.y_pad;

    resized.copyTo(canvas(cv::Rect(left, top, new_w, new_h)));
    cv::cvtColor(canvas, canvas, cv::COLOR_BGR2RGB);
    return canvas;
}

void YoloPose::softmax(float *input, int size)
{
    float max_val = input[0];
    for (int i = 1; i < size; ++i)
    {
        if (input[i] > max_val)
            max_val = input[i];
    }
    float sum_exp = 0.0f;
    for (int i = 0; i < size; ++i)
    {
        input[i] = expf(input[i] - max_val);
        sum_exp += input[i];
    }
    for (int i = 0; i < size; ++i)
    {
        input[i] /= sum_exp;
    }
}

float YoloPose::box_iou(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1, float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0f);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0f);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) + (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.f ? 0.f : (i / u);
}

int YoloPose::process_fp32(float *input, int grid_h, int grid_w, int stride,
                           std::vector<float> &boxes, std::vector<float> &boxScores, std::vector<int> &classId,
                           int32_t zp, float scale, int index)
{
    int input_loc_len = 64;
    int obj_class_num = 1;
    int validCount = 0;
    float thres_fp = unsigmoid(obj_thresh_);

    for (int h = 0; h < grid_h; h++)
    {
        for (int w = 0; w < grid_w; w++)
        {
            for (int a = 0; a < obj_class_num; a++)
            {
                if (input[(input_loc_len + a) * grid_w * grid_h + h * grid_w + w] >= thres_fp)
                {
                    float box_conf_f32 = sigmoid(input[(input_loc_len + a) * grid_w * grid_h + h * grid_w + w]);
                    float loc[64];
                    for (int i = 0; i < input_loc_len; ++i)
                    {
                        loc[i] = input[i * grid_w * grid_h + h * grid_w + w];
                    }
                    for (int i = 0; i < input_loc_len / 16; ++i)
                    {
                        softmax(&loc[i * 16], 16);
                    }
                    float xywh_[4] = {0, 0, 0, 0};
                    float xywh[4] = {0, 0, 0, 0};
                    for (int dfl = 0; dfl < 16; ++dfl)
                    {
                        xywh_[0] += loc[dfl] * dfl;
                        xywh_[1] += loc[1 * 16 + dfl] * dfl;
                        xywh_[2] += loc[2 * 16 + dfl] * dfl;
                        xywh_[3] += loc[3 * 16 + dfl] * dfl;
                    }
                    xywh_[0] = (w + 0.5f) - xywh_[0];
                    xywh_[1] = (h + 0.5f) - xywh_[1];
                    xywh_[2] = (w + 0.5f) + xywh_[2];
                    xywh_[3] = (h + 0.5f) + xywh_[3];
                    xywh[0] = ((xywh_[0] + xywh_[2]) / 2) * stride;
                    xywh[1] = ((xywh_[1] + xywh_[3]) / 2) * stride;
                    xywh[2] = (xywh_[2] - xywh_[0]) * stride;
                    xywh[3] = (xywh_[3] - xywh_[1]) * stride;
                    xywh[0] = xywh[0] - xywh[2] / 2;
                    xywh[1] = xywh[1] - xywh[3] / 2;
                    boxes.push_back(xywh[0]); // x
                    boxes.push_back(xywh[1]); // y
                    boxes.push_back(xywh[2]); // w
                    boxes.push_back(xywh[3]); // h

                    // The index across all 3 scales needs to be cumulative
                    boxes.push_back(float(index + (h * grid_w) + w)); // keypoints index

                    boxScores.push_back(box_conf_f32);
                    classId.push_back(a);
                    validCount++;
                }
            }
        }
    }
    return validCount;
}

static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right, std::vector<int> &indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right)
    {
        key_index = indices[left];
        key = input[left];
        while (low < high)
        {
            while (low < high && input[high] <= key)
                high--;
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key)
                low++;
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

int YoloPose::post_process(rknn_output *outputs, YoloPoseLetterBoxInfo &lb, std::vector<AlgoResult> &results)
{
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;
    int validCount = 0;
    int index = 0;

    for (int i = 0; i < 3; i++)
    {
        int grid_h = out_attrs_[i].dims[2]; // Assuming NCHW shape from rknn outputs in process
        int grid_w = out_attrs_[i].dims[3];
        if (out_attrs_[i].fmt == RKNN_TENSOR_NHWC)
        {
            grid_h = out_attrs_[i].dims[1];
            grid_w = out_attrs_[i].dims[2];
        }
        int stride = model_h_ / grid_h;

        validCount += process_fp32((float *)outputs[i].buf, grid_h, grid_w, stride, filterBoxes, objProbs,
                                   classId, out_attrs_[i].zp, out_attrs_[i].scale, index);
        index += grid_h * grid_w;
    }

    if (validCount <= 0)
        return 0;

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i)
    {
        indexArray.push_back(i);
    }
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    for (int i = 0; i < validCount; ++i)
    {
        int n = indexArray[i];
        if (n == -1)
            continue;
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = indexArray[j];
            if (m == -1 || classId[m] != classId[n])
                continue;
            float xmin0 = filterBoxes[n * 5 + 0];
            float ymin0 = filterBoxes[n * 5 + 1];
            float xmax0 = filterBoxes[n * 5 + 0] + filterBoxes[n * 5 + 2];
            float ymax0 = filterBoxes[n * 5 + 1] + filterBoxes[n * 5 + 3];

            float xmin1 = filterBoxes[m * 5 + 0];
            float ymin1 = filterBoxes[m * 5 + 1];
            float xmax1 = filterBoxes[m * 5 + 0] + filterBoxes[m * 5 + 2];
            float ymax1 = filterBoxes[m * 5 + 1] + filterBoxes[m * 5 + 3];

            float iou = box_iou(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > nms_thresh_)
            {
                indexArray[j] = -1;
            }
        }
    }

    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1)
            continue;
        int n = indexArray[i];
        float x1 = filterBoxes[n * 5 + 0];
        float y1 = filterBoxes[n * 5 + 1];
        float w = filterBoxes[n * 5 + 2];
        float h = filterBoxes[n * 5 + 3];
        int keypoints_index = (int)filterBoxes[n * 5 + 4];

        AlgoResult res;
        res.class_id = classId[n];
        res.label = "person"; // Pose usually only supports person class
        res.score = objProbs[n];

        // yolo.cpp uses: res.box.x = (int)((cx - rw/2.0f - lb.dw) / lb.ratio);
        // We do identically. Here x1 is already center - width/2.
        res.box.x = std::max(0, (int)((x1 - lb.x_pad) / lb.scale));
        res.box.y = std::max(0, (int)((y1 - lb.y_pad) / lb.scale));
        res.box.width = std::max(0, (int)(w / lb.scale));
        res.box.height = std::max(0, (int)(h / lb.scale));

        res.keypoints.resize(17);
        // Ensure out_attrs_[3] has the right dimensions. If not, it might crash or read junk.
        int total_grid_pts = 8400; // YOLOv8 pose default is usually 8400

        /* keypoints extraction:
         * The standalone yolo-pose repo accesses this as 1D array:
         *   j*3*8400 + 0*8400 for X,  1*8400 for Y. */
        if (out_attrs_.size() > 3)
        {
            float *kpts_buf = (float *)outputs[3].buf;
            for (int j = 0; j < 17; ++j)
            {
                int x_offset = j * 3 * total_grid_pts + 0 * total_grid_pts + keypoints_index;
                int y_offset = j * 3 * total_grid_pts + 1 * total_grid_pts + keypoints_index;

                float kx = (kpts_buf[x_offset] - lb.x_pad) / lb.scale;
                float ky = (kpts_buf[y_offset] - lb.y_pad) / lb.scale;
                res.keypoints[j] = cv::Point2f(kx, ky);
            }
        }
        results.push_back(res);
    }
    return 0;
}

bool YoloPose::infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (frame.empty())
        return false;

    auto t0 = std::chrono::steady_clock::now();
    YoloPoseLetterBoxInfo lb;
    cv::Mat input_mat = preprocess(frame, lb);
    auto t1 = std::chrono::steady_clock::now();

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = model_w_ * model_h_ * model_c_;
    inputs[0].buf = input_mat.data;
    if (rknn_inputs_set(ctx_, 1, inputs) < 0)
        return false;

    if (rknn_run(ctx_, NULL) < 0)
        return false;
    auto t2 = std::chrono::steady_clock::now();

    rknn_output outputs[io_num_out_];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num_out_; i++)
    {
        outputs[i].want_float = 1; // Force fp32 output so we don't have to deal with quantization math
    }
    if (rknn_outputs_get(ctx_, io_num_out_, outputs, NULL) < 0)
        return false;

    results.clear();
    post_process(outputs, lb, results);
    auto t3 = std::chrono::steady_clock::now();

    rknn_outputs_release(ctx_, io_num_out_, outputs);

    if (perf)
    {
        perf->preprocess_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}

bool YoloPose::infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (!zero_copy_input_enabled_ || !in_mem_ || in_mem_->fd < 0)
        return false;

    /* RGA FD→FD 路径已直接输出 RGB888 到 NPU 显存, 无需 CPU 通道交换 */

    auto t1 = std::chrono::steady_clock::now();

    /* NPU 输入显存已被 RGA 直写, 直接推理 */
    if (rknn_run(ctx_, NULL) < 0)
    {
        printf("[YoloPose] rknn_run (zero-copy) failed\n");
        return false;
    }
    auto t2 = std::chrono::steady_clock::now();

    for (int i = 0; i < io_num_out_; i++)
    {
        memset(&rknn_outputs_cache_[i], 0, sizeof(rknn_output));
        rknn_outputs_cache_[i].index = i;
        rknn_outputs_cache_[i].want_float = 1; /* pose 强制 fp32 输出 */
    }
    if (rknn_outputs_get(ctx_, io_num_out_, rknn_outputs_cache_.data(), NULL) < 0)
    {
        printf("[YoloPose] rknn_outputs_get (zero-copy) failed\n");
        return false;
    }

    /* 零拷贝下 RGA 直接 stretch 到 model 尺寸, 没有 letterbox padding.
     * 与 v5 / v8det 路径保持一致: scale=1.0, x_pad=0, y_pad=0.
     * 后处理产出的 box / keypoints 坐标在模型空间, 由调用方反推回原图. */
    YoloPoseLetterBoxInfo lb;
    lb.scale = 1.0f;
    lb.x_pad = 0.0f;
    lb.y_pad = 0.0f;

    results.clear();
    post_process(rknn_outputs_cache_.data(), lb, results);

    rknn_outputs_release(ctx_, io_num_out_, rknn_outputs_cache_.data());

    auto t3 = std::chrono::steady_clock::now();
    if (perf)
    {
        /* preprocess_ms 已由调用方统计 RGA 耗时; 此处仅填充 NPU + 后处理 */
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}
