// 课程4:ROI区域信息获取示例
// 难度:★☆☆☆☆
#include "logic/core/logic_common.h"

static void logic_course_04(ChannelContext *ctx)
{
    if (!ctx)
    {
        return;
    }
    /*======================= 基本信息获取 ==========================*/
    const std::vector<RoiZone> *roi_info = ctx->rois;
    printf("========================================\n");
    printf("视频流%d共配置了%zu个roi区域\n", ctx->chnId, roi_info->size());
    for (size_t i = 0; i < roi_info->size(); i++)
    {
        const RoiZone single_zone = (*roi_info)[i];
        printf("\t第%zu个区域:名称%s,顶点数量%zu,第1个点的坐标为(%d,%d)\n", i + 1, single_zone.name.c_str(),
               single_zone.polygon.size(), single_zone.polygon[0].x, single_zone.polygon[0].y);
    }
}

REGISTER_LOGIC(logic_course_04);
