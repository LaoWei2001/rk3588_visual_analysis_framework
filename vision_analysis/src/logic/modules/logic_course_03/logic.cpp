// 课程3：模型检测结果调用
// 难度:★☆☆☆☆
#include "logic/core/logic_common.h"

static void logic_course_03(ChannelContext *ctx)
{
    if (!ctx)
    {
        return;
    }
    /* =================== 通道配置的模型信息 ========================*/
    // 调取本通道的配置信息变量ctx->config, 其中包含了模型的相关信息。
    const ChannelConfig *cfg = ctx->config;
    printf("==================================================\n");
    printf("当前视频通道%d 应用了%zu种模型\n", ctx->chnId, cfg->models.size());
    for (size_t i = 0; i < cfg->models.size(); i++)
    {
        const ChannelModelConfig *model_cfg = &(cfg->models[i]);
        printf("\t模型%zu 类型为%s 模型路径为%s 使用npu%d进行推理\n", i + 1, model_cfg->model_type.c_str(),
               model_cfg->model_path.c_str(), model_cfg->npu_core);
    }

    /* ==================== 本通道模型的检测结果 ======================*/
    const std::vector<AlgoResult> *results = ctx->results;
    size_t target_nums = results->size();
    printf("当前画面共检测到%zu个目标\n", target_nums);
    for (size_t i = 0; i < target_nums; i++)
    {
        const AlgoResult *single_result = &((*results)[i]);
        printf("\t目标%zu 类别为%s 置信度%.2f 目标框左上角的坐标为(%d,%d) 高%d 宽%d\n", i + 1,
               single_result->label.c_str(), single_result->score, single_result->box.x, single_result->box.y,
               single_result->box.height, single_result->box.width);
    }
    printf("==================================================\n");
}

REGISTER_LOGIC(logic_course_03);
