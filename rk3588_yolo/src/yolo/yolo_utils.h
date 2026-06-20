#pragma once
#include <cmath>
#include <string>
#include <vector>
#include <fstream>

/*======================== 通用 LetterBox 信息 ========================*/
struct LetterBoxInfo {
    float ratio = 1.0f;
    int dw = 0;
    int dh = 0;
};

/*======================== 量化工具函数 ========================*/

// 通用激活函数
inline float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }
inline float unsigmoid(float y) { return -1.0f * logf((1.0f / y) - 1.0f); }

// 量化工具
inline int32_t clip(float val, float min, float max) {
    return val <= min ? min : (val >= max ? max : val);
}

inline int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    return (int8_t)clip((f32 / scale) + zp, -128, 127);
}

inline float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

// 标签加载
inline void load_label_file(const std::string& path, std::vector<std::string>& labels) {
    labels.clear();
    std::ifstream ifs(path);
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) labels.push_back(line);
    }
}

/*======================== IoU 计算 (供 Tracker / NMS / 后处理共用) ========================*/

// cv::Rect<int> 版本
inline float compute_iou(const cv::Rect &a, const cv::Rect &b)
{
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);
    int inter_w = std::max(0, x2 - x1);
    int inter_h = std::max(0, y2 - y1);
    float inter = static_cast<float>(inter_w * inter_h);
    float area_a = static_cast<float>(a.width * a.height);
    float area_b = static_cast<float>(b.width * b.height);
    float uni = area_a + area_b - inter;
    if (uni <= 0.0f)
        return 0.0f;
    return inter / uni;
}

// cv::Rect_<float> 版本 (供 Tracker 使用)
inline float compute_iou(const cv::Rect_<float> &a, const cv::Rect_<float> &b)
{
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.width, b.x + b.width);
    float y2 = std::min(a.y + a.height, b.y + b.height);
    float inter_w = std::max(0.0f, x2 - x1);
    float inter_h = std::max(0.0f, y2 - y1);
    float inter = inter_w * inter_h;
    float area_a = a.width * a.height;
    float area_b = b.width * b.height;
    float uni = area_a + area_b - inter;
    if (uni <= 0.0f)
        return 0.0f;
    return inter / uni;
}

// 参数版本 (供 yolov8det 使用)
inline float compute_iou(float xmin0, float ymin0, float xmax0, float ymax0,
                          float xmin1, float ymin1, float xmax1, float ymax1)
{
    float x1 = std::max(xmin0, xmin1);
    float y1 = std::max(ymin0, ymin1);
    float x2 = std::min(xmax0, xmax1);
    float y2 = std::min(ymax0, ymax1);
    float inter_w = std::max(0.0f, x2 - x1);
    float inter_h = std::max(0.0f, y2 - y1);
    float inter = inter_w * inter_h;
    float area_a = (xmax0 - xmin0) * (ymax0 - ymin0);
    float area_b = (xmax1 - xmin1) * (ymax1 - ymin1);
    float uni = area_a + area_b - inter;
    if (uni <= 0.0f)
        return 0.0f;
    return inter / uni;
}
