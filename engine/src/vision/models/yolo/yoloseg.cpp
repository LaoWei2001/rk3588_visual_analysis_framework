#include "yoloseg.h"
#include "detection_order.h"
#include "rknn_query_utils.h"
#include "vision/inference/npu_execution.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <rga/im2d.h>
#include <set>

static const int ANCHORS[3][6] = {{10, 13, 16, 30, 33, 23}, {30, 61, 62, 45, 59, 119}, {116, 90, 156, 198, 373, 326}};

static inline int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? (int)val : max) : min;
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                              float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0f);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0f);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) + (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int nms_seg(int validCount, std::vector<float> &outputLocations, std::vector<int> &classIds,
                   std::vector<int> &order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        if (order[i] == -1 || classIds[order[i]] != filterId)
        {
            continue;
        }
        int n = order[i];
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
            {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold)
            {
                order[j] = -1;
            }
        }
    }
    return 0;
}

// qnt_f32_to_affine / deqnt_affine_to_f32 使用 yolo_utils.h 中的全局 inline 版本

YoloSeg::YoloSeg(const std::string &model_path, const std::string &label_path, int core_mask, float obj_thresh,
                 float nms_thresh)
{
    obj_thresh_ = obj_thresh;
    nms_thresh_ = nms_thresh;
    load_labels(label_path);
    try
    {
        init_rknn(model_path, core_mask);
        query_model_info();
        init_zero_copy_input();
        outputs_.initialize(ctx_, out_attrs_, !is_quant_, zero_copy_input_enabled_);
        printf("[YoloSeg] native outputs=%d\n", outputs_.native());
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

YoloSeg::~YoloSeg()
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

void YoloSeg::load_labels(const std::string &path)
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

void YoloSeg::init_rknn(const std::string &model_path, int core_mask)
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
        printf("[YoloSeg] warn: set core mask failed, fallback single core\n");
    }
    else
    {
        printf("[YoloSeg] use core mask 0x%x\n", core_mask);
    }
}

void YoloSeg::query_model_info()
{
    rknn_input_output_num io_num{};
    model_io::query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    model_io::validate_io(io_num);
    if (io_num.n_output != 7)
        throw std::runtime_error("Unsupported yoloseg output count");
    io_num_in_ = io_num.n_input;
    io_num_out_ = io_num.n_output;

    memset(&in_attr_, 0, sizeof(in_attr_));
    in_attr_.index = 0;
    model_io::query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
    model_io::validate_image(in_attr_);
    if (in_attr_.fmt != RKNN_TENSOR_NHWC)
        throw std::runtime_error("This decoder requires NHWC input");
    model_w_ = in_attr_.dims[2]; // nhwc
    model_h_ = in_attr_.dims[1];

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

    is_quant_ = std::all_of(out_attrs_.begin(), out_attrs_.end(), [](const rknn_tensor_attr &a) {
        return a.type == RKNN_TENSOR_INT8 && a.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
    });
    const auto &proto = out_attrs_.at(6);
    model_io::validate_proto(proto, model_w_, model_h_);
    proto_channels_ = proto.dims[1];
    proto_height_ = proto.dims[2];
    proto_width_ = proto.dims[3];
    for (int i = 0; i < 3; ++i)
    {
        const auto &box = out_attrs_[i * 2], &coeff = out_attrs_[i * 2 + 1];
        const unsigned h = model_h_ / (8 << i), w = model_w_ / (8 << i);
        if (model_h_ % (8 << i) || model_w_ % (8 << i) || box.fmt != RKNN_TENSOR_NCHW || box.dims[0] != 1 ||
            box.dims[1] <= 15 || box.dims[1] % 3 || box.dims[1] != out_attrs_[0].dims[1] || box.dims[2] != h ||
            box.dims[3] != w || coeff.fmt != RKNN_TENSOR_NCHW || coeff.dims[0] != 1 ||
            coeff.dims[1] != unsigned(3 * proto_channels_) || coeff.dims[2] != h || coeff.dims[3] != w)
            throw std::runtime_error("Unsupported segmentation detection/mask head");
    }
    proto_cache_.resize(proto_channels_ * proto_height_ * proto_width_);

    if (!out_attrs_.empty())
    {
        const rknn_tensor_attr &det_attr = out_attrs_[0];
        int det_channel = (det_attr.fmt == RKNN_TENSOR_NHWC) ? det_attr.dims[3] : det_attr.dims[1];
        int inferred_classes = det_channel / 3 - 5;
        if (inferred_classes > 0)
        {
            num_classes_ = inferred_classes;
        }
    }
    if (num_classes_ <= 0)
    {
        num_classes_ = labels_.empty() ? 80 : (int)labels_.size();
    }

    // In yolov5_seg, there are 7 outputs usually. The first 6 are pairs of box and segments, last is proto.
    printf("[YoloSeg] model %dx%d, classes=%d, is_quant=%d, io_num_out=%d\n", model_w_, model_h_, num_classes_,
           is_quant_, io_num_out_);
}

bool YoloSeg::init_zero_copy_input()
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
        printf("[YoloSeg] zero-copy input disabled: rknn_create_mem failed\n");
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
        printf("[YoloSeg] zero-copy input disabled: rknn_set_io_mem failed(%d)\n", ret);
        rknn_destroy_mem(ctx_, in_mem_);
        in_mem_ = nullptr;
        return false;
    }

    zero_copy_input_enabled_ = true;
    printf("[YoloSeg] zero-copy input enabled, mem=%u bytes\n", alloc_size);

    im_handle_param_t rga_dst_param{};
    rga_dst_param.width = static_cast<uint32_t>(model_w_);
    rga_dst_param.height = static_cast<uint32_t>(model_h_);
    rga_dst_param.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(in_mem_->fd, &rga_dst_param));
    if (input_rga_handle_ == 0)
        printf("[YoloSeg] Warning: RGA dst handle cache failed, will import per-frame\n");
    else
        printf("[YoloSeg] RGA dst handle cached (handle=%d)\n", input_rga_handle_);

    return true;
}

cv::Mat YoloSeg::preprocess(cv::Mat &img, LetterBoxInfo &lb)
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
    cv::Mat canvas = cv::Mat::zeros(model_h_, model_w_, CV_8UC3);
    resized.copyTo(canvas(cv::Rect(lb.dw, lb.dh, nw, nh)));
    cv::cvtColor(canvas, canvas, cv::COLOR_BGR2RGB);

    // In standalone, they pad both sides symmetrically
    // The previous implementation gives all padding to bottom/right
    // Let's stick with the math provided here matching YOLO implementation, but ensure decoding matches.

    return canvas;
}

int YoloSeg::process_i8(rknn_output *all_input, int input_id, const int *anchor, int grid_h, int grid_w, int height,
                        int width, int stride, int class_num, std::vector<float> &boxes, std::vector<float> &segments,
                        float *proto, std::vector<float> &objProbs, std::vector<int> &classId, float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    const int prop_box_size = 5 + class_num;

    if (input_id % 2 == 1)
    {
        return validCount;
    }

    if (input_id == 6)
    {
        int8_t *input_proto = (int8_t *)all_input[input_id].buf;
        int32_t zp_proto = out_attrs_[input_id].zp;
        float scale_proto = out_attrs_[input_id].scale;
        for (int i = 0; i < proto_channels_ * proto_height_ * proto_width_; i++)
        {
            proto[i] = deqnt_affine_to_f32(input_proto[i], zp_proto, scale_proto);
        }
        return validCount;
    }

    int8_t *input = (int8_t *)all_input[input_id].buf;
    int8_t *input_seg = (int8_t *)all_input[input_id + 1].buf;
    int32_t zp = out_attrs_[input_id].zp;
    float scale = out_attrs_[input_id].scale;
    int32_t zp_seg = out_attrs_[input_id + 1].zp;
    float scale_seg = out_attrs_[input_id + 1].scale;

    int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);

    for (int a = 0; a < 3; a++)
    {
        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                int8_t box_confidence = input[(prop_box_size * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence >= thres_i8)
                {
                    int offset = (prop_box_size * a) * grid_len + i * grid_w + j;
                    int offset_seg = (proto_channels_ * a) * grid_len + i * grid_w + j;
                    int8_t *in_ptr = input + offset;
                    int8_t *in_ptr_seg = input_seg + offset_seg;

                    float box_x = (deqnt_affine_to_f32(*in_ptr, zp, scale)) * 2.0f - 0.5f;
                    float box_y = (deqnt_affine_to_f32(in_ptr[grid_len], zp, scale)) * 2.0f - 0.5f;
                    float box_w = (deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale)) * 2.0f;
                    float box_h = (deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale)) * 2.0f;
                    box_x = (box_x + j) * (float)stride;
                    box_y = (box_y + i) * (float)stride;
                    box_w = box_w * box_w * (float)anchor[a * 2];
                    box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                    box_x -= (box_w / 2.0f);
                    box_y -= (box_h / 2.0f);

                    int8_t maxClassProbs = in_ptr[5 * grid_len];
                    int maxClassId = 0;
                    for (int k = 1; k < class_num; ++k)
                    {
                        int8_t prob = in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs)
                        {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }

                    float box_conf_f32 = deqnt_affine_to_f32(box_confidence, zp, scale);
                    float class_prob_f32 = deqnt_affine_to_f32(maxClassProbs, zp, scale);
                    float limit_score = box_conf_f32 * class_prob_f32;
                    if (limit_score > threshold)
                    {
                        for (int k = 0; k < proto_channels_; k++)
                        {
                            float seg_element_fp = deqnt_affine_to_f32(in_ptr_seg[(k)*grid_len], zp_seg, scale_seg);
                            segments.push_back(seg_element_fp);
                        }

                        objProbs.push_back((deqnt_affine_to_f32(maxClassProbs, zp, scale)) *
                                           (deqnt_affine_to_f32(box_confidence, zp, scale)));
                        classId.push_back(maxClassId);
                        validCount++;
                        boxes.push_back(box_x);
                        boxes.push_back(box_y);
                        boxes.push_back(box_w);
                        boxes.push_back(box_h);
                    }
                }
            }
        }
    }
    return validCount;
}

int YoloSeg::process_fp32(rknn_output *all_input, int input_id, const int *anchor, int grid_h, int grid_w, int height,
                          int width, int stride, int class_num, std::vector<float> &boxes, std::vector<float> &segments,
                          float *proto, std::vector<float> &objProbs, std::vector<int> &classId, float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    const int prop_box_size = 5 + class_num;

    if (input_id % 2 == 1)
    {
        return validCount;
    }

    if (input_id == 6)
    {
        float *input_proto = (float *)all_input[input_id].buf;
        for (int i = 0; i < proto_channels_ * proto_height_ * proto_width_; i++)
        {
            proto[i] = input_proto[i];
        }
        return validCount;
    }

    float *input = (float *)all_input[input_id].buf;
    float *input_seg = (float *)all_input[input_id + 1].buf;

    for (int a = 0; a < 3; a++)
    {
        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                float box_confidence = input[(prop_box_size * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence >= threshold)
                {
                    int offset = (prop_box_size * a) * grid_len + i * grid_w + j;
                    int offset_seg = (proto_channels_ * a) * grid_len + i * grid_w + j;
                    float *in_ptr = input + offset;
                    float *in_ptr_seg = input_seg + offset_seg;

                    float box_x = *in_ptr * 2.0f - 0.5f;
                    float box_y = in_ptr[grid_len] * 2.0f - 0.5f;
                    float box_w = in_ptr[2 * grid_len] * 2.0f;
                    float box_h = in_ptr[3 * grid_len] * 2.0f;
                    box_x = (box_x + j) * (float)stride;
                    box_y = (box_y + i) * (float)stride;
                    box_w = box_w * box_w * (float)anchor[a * 2];
                    box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                    box_x -= (box_w / 2.0f);
                    box_y -= (box_h / 2.0f);

                    float maxClassProbs = in_ptr[5 * grid_len];
                    int maxClassId = 0;
                    for (int k = 1; k < class_num; ++k)
                    {
                        float prob = in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs)
                        {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }
                    float limit_score = maxClassProbs * box_confidence;
                    if (limit_score > threshold)
                    {
                        for (int k = 0; k < proto_channels_; k++)
                        {
                            float seg_element_f32 = in_ptr_seg[(k)*grid_len];
                            segments.push_back(seg_element_f32);
                        }

                        objProbs.push_back(limit_score);
                        classId.push_back(maxClassId);
                        validCount++;
                        boxes.push_back(box_x);
                        boxes.push_back(box_y);
                        boxes.push_back(box_w);
                        boxes.push_back(box_h);
                    }
                }
            }
        }
    }
    return validCount;
}

int YoloSeg::post_process(rknn_output *outputs, LetterBoxInfo &lb, int ori_in_width, int ori_in_height,
                          std::vector<AlgoResult> &results)
{
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;

    std::vector<float> filterSegments;
    float *proto = proto_cache_.data();
    std::vector<float> filterSegments_by_nms;
    filterBoxes.reserve(256 * 4);
    objProbs.reserve(256);
    classId.reserve(256);
    filterSegments.reserve(256 * proto_channels_);

    int validCount = 0;
    int stride = 0;
    int grid_h = 0;
    int grid_w = 0;

    if (num_classes_ <= 0 || io_num_out_ < 7 || out_attrs_.size() < 7)
    {
        return 0;
    }

    /* 0/2/4 是三个检测头，1/3/5 是对应 mask 系数，6 是 proto。
     *
     * 旧循环还会调用三个必然立即返回的奇数分支，并在 i=6 时形成 ANCHORS[3] 越界地址。 */
    for (int i = 0; i < 6; i += 2)
    {
        grid_h = out_attrs_[i].dims[2];
        grid_w = out_attrs_[i].dims[3];
        stride = model_h_ / grid_h;

        if (is_quant_)
        {
            validCount += process_i8(outputs, i, ANCHORS[i / 2], grid_h, grid_w, model_h_, model_w_, stride,
                                     num_classes_, filterBoxes, filterSegments, proto, objProbs, classId, obj_thresh_);
        }
        else
        {
            validCount +=
                process_fp32(outputs, i, ANCHORS[i / 2], grid_h, grid_w, model_h_, model_w_, stride, num_classes_,
                             filterBoxes, filterSegments, proto, objProbs, classId, obj_thresh_);
        }
    }

    const int proto_grid_h = out_attrs_[6].dims[2];
    const int proto_grid_w = out_attrs_[6].dims[3];
    if (is_quant_)
        process_i8(outputs, 6, nullptr, proto_grid_h, proto_grid_w, model_h_, model_w_, 1, num_classes_, filterBoxes,
                   filterSegments, proto, objProbs, classId, obj_thresh_);
    else
        process_fp32(outputs, 6, nullptr, proto_grid_h, proto_grid_w, model_h_, model_w_, 1, num_classes_, filterBoxes,
                     filterSegments, proto, objProbs, classId, obj_thresh_);

    if (validCount <= 0)
    {
        return 0;
    }
    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i)
    {
        indexArray.push_back(i);
    }

    sort_detection_indices(objProbs, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));

    for (auto c : class_set)
    {
        nms_seg(validCount, filterBoxes, classId, indexArray, c, nms_thresh_);
    }

    std::vector<AlgoResult> filtered_results;
    std::vector<cv::Rect_<float>> validModelBoxes; // Holds the model-size bounding boxes

    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1) // Removed by NMS
        {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[n]; // Fixed to use proper index

        for (int k = 0; k < proto_channels_; k++)
        {
            filterSegments_by_nms.push_back(filterSegments[n * proto_channels_ + k]);
        }

        validModelBoxes.push_back(cv::Rect_<float>(x1, y1, x2 - x1, y2 - y1));

        AlgoResult res;
        // The original box mapping maps from letterboxed back to original
        // We will apply the same mapping YOLO does: res.box.x = (cx - rw/2 - dw) / ratio
        // Which is exactly ((x1 - dw) / ratio)
        res.box.x = clamp((int)((x1 - lb.dw) / lb.ratio), 0, ori_in_width);
        res.box.y = clamp((int)((y1 - lb.dh) / lb.ratio), 0, ori_in_height);
        res.box.width = clamp((int)((x2 - lb.dw) / lb.ratio) - res.box.x, 0, ori_in_width - res.box.x);
        res.box.height = clamp((int)((y2 - lb.dh) / lb.ratio) - res.box.y, 0, ori_in_height - res.box.y);

        // Ensure strictly non-negative dimensions
        if (res.box.width < 0)
            res.box.width = 0;
        if (res.box.height < 0)
            res.box.height = 0;

        res.score = obj_conf;
        res.class_id = id;
        res.label = (id < (int)labels_.size()) ? labels_[id] : std::to_string(id);

        filtered_results.push_back(res);
    }

    int boxes_num = filtered_results.size();
    if (boxes_num == 0)
        return 0;

    /* Mask generation: OpenCV GEMM 会使用平台优化内核（RK3588 上可走 NEON），替代
     * boxes × 32 ×
     * 25600 的手写标量三重循环。 */
    cv::Mat coefficients(boxes_num, proto_channels_, CV_32FC1, filterSegments_by_nms.data());
    cv::Mat proto_matrix(proto_channels_, proto_height_ * proto_width_, CV_32FC1, proto);
    cv::Mat matmul_out;
    cv::gemm(coefficients, proto_matrix, 1.0, cv::Mat(), 0.0, matmul_out);
    float *matmul_ptr = reinterpret_cast<float *>(matmul_out.data);

    // 2. resize to model_w_ x model_h_
    cv::Mat seg_mask(boxes_num, model_h_ * model_w_, CV_32FC1);
    for (int b = 0; b < boxes_num; b++)
    {
        cv::Mat src_image(proto_height_, proto_width_, CV_32F, &matmul_ptr[b * proto_height_ * proto_width_]);
        cv::Mat dst_image = seg_mask.row(b).reshape(1, model_h_);
        cv::resize(src_image, dst_image, cv::Size(model_w_, model_h_), 0, 0, cv::INTER_LINEAR);
    }

    // 3. Crop mask
    // This part generates a combined mask for the whole image (model_h_ x model_w_)
    cv::Mat all_mask_in_one = cv::Mat::zeros(model_h_, model_w_, CV_8UC1);
    // NMS may have changed the boxes, we must use the ones inside the model
    // The mask generation happens inside model_w_ x model_h_ frame
    // original box on `model_w_ x model_h_`: x1, y1, x2, y2 from filterBoxes (using indices arrays)
    for (int b = 0; b < boxes_num; b++)
    {
        // Reconstruct box within the model image space before reversing padding
        // Because algorithm filters masks using the bounding box
        float x1 = validModelBoxes[b].x;
        float y1 = validModelBoxes[b].y;
        float x2 = x1 + validModelBoxes[b].width;
        float y2 = y1 + validModelBoxes[b].height;
        int cls_id = filtered_results[b].class_id;

        // Ensure bounds to avoid unnecessary loop checks
        int x_start = clamp((int)x1, 0, model_w_);
        int y_start = clamp((int)y1, 0, model_h_);
        int x_end = clamp((int)x2, 0, model_w_);
        int y_end = clamp((int)y2, 0, model_h_);

        if (x_end <= x_start || y_end <= y_start)
            continue;
        const cv::Rect roi(x_start, y_start, x_end - x_start, y_end - y_start);
        cv::Mat object_mask = seg_mask.row(b).reshape(1, model_h_)(roi);
        cv::Mat output_roi = all_mask_in_one(roi);
        cv::Mat positive = object_mask > 0.0f;
        cv::Mat unassigned = output_roi == 0;
        cv::bitwise_and(positive, unassigned, positive);
        output_roi.setTo(cv::Scalar(cls_id + 1), positive);
    }

    // 4. Reverse the padding and scale back to the original image dimensions
    // We only take the part of all_mask_in_one that maps to the original image
    int cropped_width = model_w_ - lb.dw * 2;
    int cropped_height = model_h_ - lb.dh * 2;
    // ensure within bounds
    if (cropped_width <= 0 || cropped_height <= 0)
    {
        for (auto &r : filtered_results)
            results.push_back(r);
        return validCount;
    }

    cv::Mat cropped_seg = all_mask_in_one(cv::Rect(lb.dw, lb.dh, cropped_width, cropped_height)).clone();
    cv::Mat final_mask_out;
    cv::resize(cropped_seg, final_mask_out, cv::Size(ori_in_width, ori_in_height), 0, 0,
               cv::INTER_NEAREST); // use NEAREST to preserve class ids

    // Attach mask to the first result to avoid passing big arrays across structures separately
    // The mask covers the full original image dimensions.
    if (!filtered_results.empty())
    {
        filtered_results[0].boxMask = final_mask_out;
    }

    for (auto &r : filtered_results)
    {
        results.push_back(r);
    }
    return validCount;
}

bool YoloSeg::infer(cv::Mat &frame, std::vector<AlgoResult> &results, ModelPerfStat *perf)
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
        printf("[YoloSeg] RKNN execution failed: %d\n", ret);
        return false;
    }
    std::vector<AlgoResult> candidates;
    candidates.reserve(128);
    post_process(outputs_.data(), lb, frame.cols, frame.rows, candidates);
    results.swap(candidates);
    const auto t3 = std::chrono::steady_clock::now();
    if (perf)
    {
        perf->preprocess_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}

bool YoloSeg::infer_zero_copy(std::vector<AlgoResult> &results, ModelPerfStat *perf)
{
    results.clear();
    if (!zero_copy_input_enabled_ || !in_mem_ || in_mem_->fd < 0)
        return false;
    const auto t1 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t2;
    const int ret = outputs_.execute(npu_core_mask_, nullptr, 0, t2);
    if (ret < 0)
    {
        printf("[YoloSeg] RKNN DMA execution failed: %d\n", ret);
        return false;
    }
    // RGA stretches to the model canvas; pipeline maps results to the source.
    LetterBoxInfo lb;
    lb.ratio = 1.f;
    lb.dw = 0;
    lb.dh = 0;
    std::vector<AlgoResult> candidates;
    candidates.reserve(128);
    post_process(outputs_.data(), lb, model_w_, model_h_, candidates);
    results.swap(candidates);
    const auto t3 = std::chrono::steady_clock::now();
    if (perf)
    {
        perf->infer_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
        perf->postprocess_ms = std::chrono::duration<float, std::milli>(t3 - t2).count();
    }
    return true;
}
