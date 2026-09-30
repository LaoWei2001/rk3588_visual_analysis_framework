// 课程: GPIO 引脚控制
// 实现效果: 画面中检测到指定类别目标时, 驱动输出引脚(继电器吸合/指示灯点亮), 目标消失后恢复
// 难度: ★★☆☆☆
// 知识点:
//   1. 底层能力封装 src/gpio/gpio.h 的懒加载用法 —— 无需预注册, 逻辑里直接调用
//   2. ctx->state 跨帧持久化 —— 只在电平需要变化时写引脚, 避免逐帧重复操作
//   3. 参数从 logic.json 读取 —— ctx->param_string / ctx->param_int
//
// 接线说明: 把继电器/指示灯模块的 IN 脚接到输出引脚(默认 GPIO6_A2),
//           该引脚与控制板共地(GND)即可。

#include "logic/core/logic_common.h"
#include "gpio/gpio.h"

struct GpioDemoState
{
    int last_level = -1; /* 上一次实际写入的电平, -1 表示尚未写入成功 */
};

static void logic_course_gpio(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
        return;

    // 跨帧状态: 每个通道独立一份, 首次进入时创建
    if (!*ctx->state)
        *ctx->state = std::make_shared<GpioDemoState>();
    std::shared_ptr<GpioDemoState> state = std::static_pointer_cast<GpioDemoState>(*ctx->state);

    // 从 logic.json 读取参数(启动/热重载时已补默认值并校验)
    std::string pin_name = ctx->param_string("out_pin");
    std::string target_label = ctx->param_string("target_label");
    int active_level = (int)ctx->param_int("active_level");

    if (pin_name.empty()) {
        draw_text(ctx, "GPIO演示: 未配置输出引脚(out_pin)", cv::Point(30, 50), cv::Scalar(0, 0, 255));
        return;
    }

    // 目标存在 -> 有效电平; 不存在 -> 无效电平
    int has_target = ctx->has_target(target_label.c_str());
    int want_level = has_target ? active_level : 1 - active_level;

    // 只在电平需要变化时写引脚; 写失败保持旧值, 下一帧自动重试(错误只打印一次)
    if (want_level != state->last_level) {
        if (pin_out_val(pin_name.c_str(), want_level) == 0)
            state->last_level = want_level;
    }

    // 画面上显示当前引脚状态
    const char *level_text = (state->last_level < 0) ? "未知" : (state->last_level ? "高" : "低");
    char status[128];
    snprintf(status, sizeof(status), "GPIO %s = %s电平", pin_name.c_str(), level_text);
    draw_text(ctx, status, cv::Point(30, 50),
              has_target ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 165, 255));
    draw_text(ctx, has_target ? "检测到目标, 输出有效电平" : "无目标, 输出无效电平",
              cv::Point(30, 80), cv::Scalar(200, 200, 200));
}

REGISTER_LOGIC(logic_course_gpio);
