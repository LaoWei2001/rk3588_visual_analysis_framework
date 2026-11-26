// YOLOv8 OBB decoder, derived from Rockchip rknn_model_zoo (Apache-2.0).
// Included by yolov8det.cpp to share its checked RKNN/DMA execution lifecycle.
#pragma once

void YoloV8Det::query_obb_info()
{
    rknn_input_output_num io{};
    model_io::query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    model_io::validate_io(io);
    if (io.n_output != 4)
        throw std::runtime_error("OBB requires four outputs");
    io_num_out_ = io.n_output;
    in_attr_ = {};
    in_attr_.index = 0;
    model_io::query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
    model_io::validate_image(in_attr_);
    model_h_ = in_attr_.dims[in_attr_.fmt == RKNN_TENSOR_NCHW ? 2 : 1];
    model_w_ = in_attr_.dims[in_attr_.fmt == RKNN_TENSOR_NCHW ? 3 : 2];
    out_attrs_.resize(4);
    is_quant_ = true;
    for (int i = 0; i < 4; ++i)
    {
        auto &a = out_attrs_[i];
        a = {};
        a.index = i;
        model_io::query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        is_quant_ &= a.type == RKNN_TENSOR_INT8 && a.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
    }
    num_classes_ = int(out_attrs_[0].dims[1]) - 64;
    if (num_classes_ < 1 || num_classes_ > 10000)
        throw std::runtime_error("Invalid OBB classes");
    uint64_t cells = 0;
    for (int i = 0; i < 3; ++i)
    {
        const auto &a = out_attrs_[i];
        int stride = 8 << i;
        if (model_w_ % stride || model_h_ % stride || a.n_dims != 4 || a.fmt != RKNN_TENSOR_NCHW || a.dims[0] != 1 ||
            a.dims[1] != unsigned(64 + num_classes_) || a.dims[2] != unsigned(model_h_ / stride) ||
            a.dims[3] != unsigned(model_w_ / stride))
            throw std::runtime_error("Unsupported OBB head layout");
        cells += a.dims[2] * a.dims[3];
        for (int q = 0; q < 256; ++q)
        {
            obb_exp_[i][q] = std::exp(-q * a.scale);
            obb_scores_[i][q] = 1.f / (1.f + std::exp(-(q - 128 - a.zp) * a.scale));
        }
    }
    if (out_attrs_[3].n_elems != cells)
        throw std::runtime_error("Invalid OBB angle output");
    if (!labels_.empty() && labels_.size() != size_t(num_classes_))
        throw std::runtime_error("OBB label count differs from model classes");
    if (labels_.empty())
        for (int i = 0; i < num_classes_; ++i)
            labels_.push_back("class_" + std::to_string(i));
    obb_candidates_.reserve(1024);
    obb_kept_.reserve(128);
}

int YoloV8Det::post_process_obb(rknn_output *outputs, LetterBoxInfo &lb, std::vector<AlgoResult> &results)
{
    results.clear();
    obb_candidates_.clear();
    obb_kept_.clear();
    int angle_offset = 0;
    for (int head = 0; head < 3; ++head)
    {
        const auto &attr = out_attrs_[head];
        int gh = attr.dims[2], gw = attr.dims[3], cells = gh * gw, stride = 8 << head;
        const auto *q = static_cast<const int8_t *>(outputs[head].buf);
        const auto *f = static_cast<const float *>(outputs[head].buf);
        for (int cell = 0; cell < cells; ++cell)
        {
            bool decoded = false;
            ObbCandidate candidate{};
            for (int cls = 0; cls < num_classes_; ++cls)
            {
                int index = (64 + cls) * cells + cell;
                float score = is_quant_ ? obb_scores_[head][int(q[index]) + 128] : 1.f / (1.f + std::exp(-f[index]));
                if (score < obj_thresh_)
                    continue;
                if (!decoded)
                {
                    float distance[4]{};
                    for (int side = 0; side < 4; ++side)
                    {
                        float maximum = -INFINITY;
                        for (int k = 0; k < 16; ++k)
                        {
                            int n = (side * 16 + k) * cells + cell;
                            maximum = std::max(maximum, is_quant_ ? float(q[n]) : f[n]);
                        }
                        float sum = 0, weighted = 0;
                        for (int k = 0; k < 16; ++k)
                        {
                            int n = (side * 16 + k) * cells + cell;
                            float e = is_quant_ ? obb_exp_[head][int(maximum) - int(q[n])] : std::exp(f[n] - maximum);
                            sum += e;
                            weighted += k * e;
                        }
                        distance[side] = weighted / sum;
                    }
                    const auto &aa = out_attrs_[3];
                    float angle =
                        is_quant_ ? (int(static_cast<int8_t *>(outputs[3].buf)[angle_offset + cell]) - aa.zp) * aa.scale
                                  : static_cast<float *>(outputs[3].buf)[angle_offset + cell];
                    candidate.angle = (angle - .25f) * float(CV_PI);
                    float cs = std::cos(candidate.angle), sn = std::sin(candidate.angle);
                    float dx = (distance[2] - distance[0]) * .5f, dy = (distance[3] - distance[1]) * .5f;
                    candidate.cx = (cell % gw + .5f + dx * cs - dy * sn) * stride;
                    candidate.cy = (cell / gw + .5f + dx * sn + dy * cs) * stride;
                    candidate.w = (distance[0] + distance[2]) * stride;
                    candidate.h = (distance[1] + distance[3]) * stride;
                    // Gaussian rectangle covariance for ProbIoU, cached once per candidate.
                    float a = candidate.w * candidate.w / 12.f, b = candidate.h * candidate.h / 12.f;
                    candidate.a = a * cs * cs + b * sn * sn;
                    candidate.b = a * sn * sn + b * cs * cs;
                    candidate.c = (a - b) * cs * sn;
                    candidate.determinant = std::max(0.f, candidate.a * candidate.b - candidate.c * candidate.c);
                    decoded = true;
                }
                candidate.score = score;
                candidate.cls = cls;
                obb_candidates_.push_back(candidate);
            }
        }
        angle_offset += cells;
    }
    std::stable_sort(obb_candidates_.begin(), obb_candidates_.end(),
                     [](const ObbCandidate &a, const ObbCandidate &b) { return a.score > b.score; });
    auto overlap = [](const ObbCandidate &x, const ObbCandidate &y) {
        const float eps = 1e-7f, a = x.a + y.a, b = x.b + y.b, c = x.c + y.c;
        float dx = x.cx - y.cx, dy = x.cy - y.cy, det = a * b - c * c;
        float t = (a * dy * dy + b * dx * dx - 2 * c * dx * dy) / (det + eps) * .25f;
        t += .5f * std::log(det / (4 * std::sqrt(x.determinant * y.determinant) + eps) + eps);
        t = std::max(eps, std::min(100.f, t));
        return 1.f - std::sqrt(1.f - std::exp(-t) + eps);
    };
    for (size_t i = 0; i < obb_candidates_.size() && obb_kept_.size() < 128; ++i)
    {
        const auto &c = obb_candidates_[i];
        bool suppressed = false;
        for (size_t k : obb_kept_)
            if (c.cls == obb_candidates_[k].cls && overlap(c, obb_candidates_[k]) > nms_thresh_)
            {
                suppressed = true;
                break;
            }
        if (suppressed)
            continue;
        obb_kept_.push_back(i);
        AlgoResult result;
        result.has_obb = true;
        result.score = c.score;
        result.class_id = c.cls;
        result.label = labels_[c.cls];
        cv::Point2f corners[4];
        cv::RotatedRect(cv::Point2f(c.cx, c.cy), cv::Size2f(c.w, c.h), c.angle * 180.f / float(CV_PI)).points(corners);
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (int j = 0; j < 4; ++j)
        {
            auto &p = result.obb_corners[j];
            p = cv::Point2f((corners[j].x - lb.dw) / lb.ratio, (corners[j].y - lb.dh) / lb.ratio);
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        result.box = cv::Rect(int(std::floor(x0)), int(std::floor(y0)), int(std::ceil(x1) - std::floor(x0)),
                              int(std::ceil(y1) - std::floor(y0)));
        results.push_back(std::move(result));
    }
    return results.size();
}
