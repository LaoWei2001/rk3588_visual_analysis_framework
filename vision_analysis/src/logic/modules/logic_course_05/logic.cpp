// 课程5:ROI区域信息与检测框信息的交互(是前4节课的内容的总结)
// 本节内容请删掉我的示例代码自行实现
// 实现效果:输出在指定的ROI区域的目标名称,置信度,且在区域内部的目标的检测框变为红色
// 难度:★★★☆☆

#include "logic/core/logic_common.h"

// 函数作用:输入某个检测结果,输出该检测结果的中心点坐标
cv::Point det_middle_point(const AlgoResult *single_result)
{
    cv::Point middle_point;
    middle_point.x = single_result->box.x + 0.5 * single_result->box.width;
    middle_point.y = single_result->box.y + 0.5 * single_result->box.height;
    return middle_point;
}

// 函数作用:获取所有在指定roi区域中的目标
// 输入当前通道上下文和roi名字,输出包含检测结果的动态指针数组vector
std::vector<AlgoResult *> roi_targets(const ChannelContext *ctx, const char *name)
{
    std::vector<AlgoResult *> result;
    int roi_found = 0;
    size_t target_roi_id = 0;
    // 当前帧所有的检测结果
    std::vector<AlgoResult> *det_results = ctx->results;
    // 设置的所有检测区域
    const std::vector<RoiZone> *rois = ctx->rois;
    // 检测结果数量
    size_t obj_num = det_results->size();
    // roi区域数量
    size_t roi_num = rois->size();
    // 查找目标roi区域
    for (; target_roi_id < roi_num; target_roi_id++)
    {
        if ((*rois)[target_roi_id].name == name)
        {
            roi_found = 1;
            break;
        }
    }
    if (!roi_found)
    {
        printf("未找到名称为\"%s\"的区域\n", name);
        return {};
    }
    // 遍历每个目标
    for (size_t i = 0; i < obj_num; i++)
    {
        cv::Point middle_point = det_middle_point((&(*det_results)[i]));
        int is_in_polygon = cv::pointPolygonTest((*rois)[target_roi_id].polygon, middle_point, 0);
        if (is_in_polygon >= 0)
        {
            result.push_back(&(*det_results)[i]);
        }
    }
    return result;
}

static void logic_course_05(ChannelContext *ctx)
{
    if (!ctx)
    {
        return;
    }
    // 调用上面的函数
    std::vector<AlgoResult *> obj = roi_targets(ctx, ctx->param_string("roi_name").c_str());
    size_t obj_num = obj.size();
    printf("=======================================\n");
    for (size_t i = 0; i < obj_num; i++)
    {
        (obj[i])->box_color = cv::Scalar(0, 0, 255);
        printf("目标%zu 名称:%s\n", i + 1, (obj[i])->label.c_str());
    }
    printf("=======================================\n");
}

REGISTER_LOGIC(logic_course_05);
