#pragma once
#include "vision/models/detection_geometry.h"
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

/*======================== 通用 LetterBox 信息 ========================*/
struct LetterBoxInfo
{
    float ratio = 1.0f;
    int dw = 0;
    int dh = 0;
};

/*======================== 量化工具函数 ========================*/

// 通用激活函数
inline float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

// 量化工具
inline int32_t clip(float val, float min, float max)
{
    return val <= min ? min : (val >= max ? max : val);
}

inline int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    return (int8_t)clip((f32 / scale) + zp, -128, 127);
}

inline float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return ((float)qnt - (float)zp) * scale;
}

// 标签加载
inline void load_label_file(const std::string &path, std::vector<std::string> &labels)
{
    labels.clear();
    std::ifstream ifs(path);
    std::string line;
    while (std::getline(ifs, line))
    {
        if (!line.empty())
            labels.push_back(line);
    }
}
