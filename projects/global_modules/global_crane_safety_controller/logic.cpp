#include <control.h>
#include <events.h>
#include <global.h>
#include "safety_controller.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct CraneSafetyState
{
    ~CraneSafetyState()
    {
        if (!inference_states_captured)
            return;
        logic_control_set_channel_inference(helmet_channel_id, helmet_previous_inference != 0);
        logic_control_set_channel_inference(intrusion_channel_id, intrusion_previous_inference != 0);
    }

    crane_safety::SafetyController hardware;
    bool initialized = false;
    bool crane_moving = false;
    uint64_t scene_ready_at_ms = 0;
    bool last_hook_alarm = false;
    bool last_intrusion_alarm = false;
    bool last_helmet_alarm = false;
    bool horn_silenced = false;
    bool outputs_forced_off = false;
    bool automatic_horn_on = false;
    bool horn_test_on = false;
    bool lamp_test_on = false;
    bool inference_states_captured = false;
    int helmet_channel_id = -1;
    int intrusion_channel_id = -1;
    int helmet_previous_inference = 1;
    int intrusion_previous_inference = 1;
};

CraneSafetyState &controller_state(GlobalContext *gctx)
{
    if (!*gctx->state)
        *gctx->state = std::make_shared<CraneSafetyState>();
    return *std::static_pointer_cast<CraneSafetyState>(*gctx->state);
}

uint64_t seconds_to_ms(double seconds)
{
    return static_cast<uint64_t>(std::max(0.0, seconds) * 1000.0);
}

enum class AlgorithmDisplayState
{
    ENABLED,
    DISABLED,
    WAITING
};

void show_algorithm_state(GlobalContext *gctx, int channel_id, const char *algorithm,
                          AlgorithmDisplayState state)
{
    if (!gctx || !algorithm || channel_id < 0 || gctx->model_width <= 0 || gctx->model_height <= 0)
        return;

    const char *state_text = "等待数据";
    cv::Scalar color(0, 165, 255);
    if (state == AlgorithmDisplayState::ENABLED)
    {
        state_text = "开启";
        color = cv::Scalar(0, 220, 0);
    }
    else if (state == AlgorithmDisplayState::DISABLED)
    {
        state_text = "关闭";
        color = cv::Scalar(180, 180, 180);
    }

    char text[128];
    std::snprintf(text, sizeof(text), "%s：%s", algorithm, state_text);

    const int margin = 10;
    std::vector<DrawCommand> commands;
    commands.reserve(1);

    DrawCommand label;
    label.type = DrawCommand::TEXT;
    label.text = text;
    label.text_pos = cv::Point(gctx->model_width - margin - 10, margin + 29);
    label.text_align_right = true;
    label.font_scale = 0.72;
    label.color = color;
    label.thickness = 1;
    label.text_shadow_enabled = true;
    label.text_shadow_color = cv::Scalar(0, 0, 0);
    label.text_shadow_width = 3;
    label.target = DrawCommand::ALL;
    commands.push_back(std::move(label));

    gctx->set_channel_draw_commands(channel_id, commands);
}

crane_safety::HardwareConfig read_hardware_config(GlobalContext *gctx)
{
    crane_safety::HardwareConfig config;
    config.enabled = gctx->param_bool("hardware_enabled");
    config.horn_pin = gctx->param_string("horn_pin");
    config.horn_active_level = static_cast<int>(gctx->param_int("horn_active_level"));
    config.lamp_pin = gctx->param_string("lamp_pin");
    config.lamp_active_level = static_cast<int>(gctx->param_int("lamp_active_level"));
    return config;
}

void report_hook_alarm(GlobalContext *gctx, int channel_id, bool crane_moving,
                       const ChannelInput *input)
{
    EventRequest request;
    request.event_type = "crane_hook_outside";
    request.message = "吊钩中心超出安全圆范围";
    request.source_channel_id = channel_id;
    request.evidence_channel_ids = {channel_id};
    request.fields = {
        event_field("source_channel_id", channel_id),
        event_field("crane_moving", crane_moving),
        event_field("hook_visible", input ? input->get_bool("hook_visible") : false),
        event_field("hook_distance", input ? input->get_number("hook_distance") : 0.0),
        event_field("hook_center_x", input ? input->get_int("hook_center_x") : 0),
        event_field("hook_center_y", input ? input->get_int("hook_center_y") : 0),
    };
    report_event(gctx, request);
}

void report_intrusion_alarm(GlobalContext *gctx, int channel_id, const ChannelInput *input)
{
    EventRequest request;
    request.event_type = "crane_moving_intrusion";
    request.message = "行车运动期间投影灯区域内检测到人员";
    request.source_channel_id = channel_id;
    request.evidence_channel_ids = {channel_id};
    request.fields = {
        event_field("source_channel_id", channel_id),
        event_field("crane_moving", true),
        event_field("roi_available", input ? input->get_bool("intrusion_roi_available") : false),
        event_field("person_count", input ? input->get_int("intrusion_person_count") : 0),
    };
    report_event(gctx, request);
}

void report_helmet_alarm(GlobalContext *gctx, int channel_id, const ChannelInput *input)
{
    EventRequest request;
    request.event_type = "crane_still_no_helmet";
    request.message = "行车静止期间下方区域内检测到未佩戴安全帽人员";
    request.source_channel_id = channel_id;
    request.evidence_channel_ids = {channel_id};
    request.fields = {
        event_field("source_channel_id", channel_id),
        event_field("crane_moving", false),
        event_field("roi_available", input ? input->get_bool("helmet_roi_available") : false),
        event_field("person_count", input ? input->get_int("helmet_person_count") : 0),
        event_field("unhelmeted_count", input ? input->get_int("unhelmeted_count") : 0),
    };
    report_event(gctx, request);
}

} // namespace

static LogicActionResult global_crane_safety_controller_action(GlobalContext *gctx,
                                                               const LogicAction *action)
{
    if (!gctx || !gctx->state || !action)
        return {false, "gctx or action is null"};

    CraneSafetyState &state = controller_state(gctx);
    const crane_safety::HardwareConfig hardware_config = read_hardware_config(gctx);

    if (action->name == "silence_current_alarm")
    {
        state.horn_silenced = true;
        state.automatic_horn_on = false;
        state.horn_test_on = false;
        const bool lamp_on = !state.outputs_forced_off &&
                             (state.crane_moving || state.lamp_test_on);
        state.hardware.apply(hardware_config, lamp_on, false);
        return {true, "当前告警已消音；全部有效违规解除后将自动重新布防"};
    }
    if (action->name == "test_horn")
    {
        if (state.outputs_forced_off)
            return {true, "现场输出处于紧急关闭状态，请先恢复自动控制"};
        if (!hardware_config.enabled)
            return {true, "现场硬件输出尚未启用，未执行喇叭测试"};
        state.horn_test_on = !state.horn_test_on;
        const bool lamp_on = state.crane_moving || state.lamp_test_on;
        state.hardware.apply(hardware_config, lamp_on, state.automatic_horn_on || state.horn_test_on);
        return {true, state.horn_test_on ? "喇叭测试已开启并持续保持，再按一次关闭"
                                         : "喇叭测试已关闭，恢复自动告警控制"};
    }
    if (action->name == "test_lamp")
    {
        if (state.outputs_forced_off)
            return {true, "现场输出处于紧急关闭状态，请先恢复自动控制"};
        if (!hardware_config.enabled)
            return {true, "现场硬件输出尚未启用，未执行投影灯测试"};
        state.lamp_test_on = !state.lamp_test_on;
        state.scene_ready_at_ms = gctx->timestamp_ms +
                                  seconds_to_ms(gctx->param_float("lamp_settle_sec"));
        const bool lamp_on = state.crane_moving || state.lamp_test_on;
        state.hardware.apply(hardware_config, lamp_on, state.automatic_horn_on || state.horn_test_on);
        return {true, state.lamp_test_on ? "投影灯测试已开启并持续保持，再按一次关闭"
                                         : "投影灯测试已关闭，恢复自动行车状态控制"};
    }
    if (action->name == "force_outputs_off")
    {
        state.outputs_forced_off = true;
        state.horn_silenced = true;
        state.automatic_horn_on = false;
        state.horn_test_on = false;
        state.lamp_test_on = false;
        state.hardware.apply(hardware_config, false, false);
        return {true, "喇叭和投影灯已紧急关闭，并保持锁定"};
    }
    if (action->name == "resume_automatic")
    {
        state.outputs_forced_off = false;
        state.horn_silenced = false;
        state.automatic_horn_on = false;
        state.horn_test_on = false;
        state.lamp_test_on = false;
        state.scene_ready_at_ms = gctx->timestamp_ms +
                                  seconds_to_ms(gctx->param_float("lamp_settle_sec"));
        state.hardware.apply(hardware_config, state.crane_moving, false);
        return {true, "已解除人工锁定，恢复行车安全自动控制"};
    }

    return {false, "unsupported action: " + action->name};
}

static void global_crane_safety_controller(GlobalContext *gctx)
{
    if (!gctx || !gctx->state)
        return;

    CraneSafetyState &state = controller_state(gctx);

    const int motion_channel_id = static_cast<int>(gctx->param_int("motion_channel_id"));
    const int hook_channel_id = static_cast<int>(gctx->param_int("hook_channel_id"));
    const int helmet_channel_id = static_cast<int>(gctx->param_int("helmet_channel_id"));
    const int intrusion_channel_id = static_cast<int>(gctx->param_int("intrusion_channel_id"));

    if (!state.inference_states_captured)
    {
        state.inference_states_captured = true;
        state.helmet_channel_id = helmet_channel_id;
        state.intrusion_channel_id = intrusion_channel_id;
        state.helmet_previous_inference = logic_control_get_channel_inference(helmet_channel_id);
        state.intrusion_previous_inference = logic_control_get_channel_inference(intrusion_channel_id);
    }

    const ChannelInput *motion = gctx->input(motion_channel_id);
    const ChannelInput *hook = gctx->input(hook_channel_id);
    const ChannelInput *helmet = gctx->input(helmet_channel_id);
    const ChannelInput *intrusion = gctx->input(intrusion_channel_id);

    /* 运动通道尚未发布有效结果时按静止处理。用户明确不要求视频断流故障策略。 */
    const bool motion_valid = motion && motion->get_bool("motion_valid");
    const bool crane_moving = motion_valid && motion->get_bool("crane_moving");
    const uint64_t settle_ms = seconds_to_ms(gctx->param_float("lamp_settle_sec"));

    if (!state.initialized || state.crane_moving != crane_moving)
    {
        state.initialized = true;
        state.crane_moving = crane_moving;
        state.scene_ready_at_ms = gctx->timestamp_ms + settle_ms;
    }

    const bool hook_alarm = hook && hook->get_bool("hook_alarm");
    const bool raw_helmet_alarm = helmet && helmet->get_bool("helmet_alarm");
    const bool raw_intrusion_alarm = intrusion && intrusion->get_bool("intrusion_alarm");
    bool scene_ready = gctx->timestamp_ms >= state.scene_ready_at_ms;
    const bool lamp_test_active = state.lamp_test_on;
    const bool horn_test_active = state.horn_test_on;
    const bool manual_lamp_override = lamp_test_active && !crane_moving;
    bool helmet_algorithm_enabled = !crane_moving && scene_ready && !manual_lamp_override;
    bool intrusion_algorithm_enabled = crane_moving && scene_ready;
    bool helmet_alarm = helmet_algorithm_enabled && raw_helmet_alarm;
    bool intrusion_alarm = intrusion_algorithm_enabled && raw_intrusion_alarm;
    if (state.horn_silenced && !state.outputs_forced_off &&
        !hook_alarm && !helmet_alarm && !intrusion_alarm)
        state.horn_silenced = false;

    bool lamp_on = crane_moving || lamp_test_active;
    state.automatic_horn_on = (!state.horn_silenced) && (hook_alarm || helmet_alarm || intrusion_alarm);
    bool horn_on = state.automatic_horn_on || horn_test_active;
    if (state.outputs_forced_off)
    {
        lamp_on = false;
        horn_on = false;
    }

    const crane_safety::HardwareConfig hardware_config = read_hardware_config(gctx);

    const crane_safety::HardwareStatus hardware =
        state.hardware.apply(hardware_config, lamp_on, horn_on);

    /* 继电器实际发生切换后重新计时，避免开灯/关灯瞬间造成误检。 */
    if (hardware.lamp_changed)
    {
        state.scene_ready_at_ms = gctx->timestamp_ms + settle_ms;
        helmet_algorithm_enabled = false;
        intrusion_algorithm_enabled = false;
        helmet_alarm = false;
        intrusion_alarm = false;
        state.automatic_horn_on = !state.outputs_forced_off && !state.horn_silenced && hook_alarm;
        horn_on = state.automatic_horn_on || horn_test_active;
        state.hardware.apply(hardware_config, lamp_on, horn_on);
    }

    /* 场景关闭不仅屏蔽最终告警，还暂停对应通道的 NPU 推理。非推理路径仍会调用
     * 通道 Logic，使其能立即清空锁存并持续发布“算法关闭/告警否”的确定状态。 */
    logic_control_set_channel_inference(helmet_channel_id, helmet_algorithm_enabled);
    logic_control_set_channel_inference(intrusion_channel_id, intrusion_algorithm_enabled);

    show_algorithm_state(gctx, motion_channel_id, "行车运动检测",
                         motion_valid ? AlgorithmDisplayState::ENABLED
                                      : AlgorithmDisplayState::WAITING);
    show_algorithm_state(gctx, hook_channel_id, "歪拉斜吊检测",
                         hook && hook->has("hook_alarm") ? AlgorithmDisplayState::ENABLED
                                                          : AlgorithmDisplayState::WAITING);
    show_algorithm_state(gctx, helmet_channel_id, "安全帽告警",
                         !helmet || !helmet->has("helmet_alarm")
                             ? AlgorithmDisplayState::WAITING
                             : (helmet_algorithm_enabled ? AlgorithmDisplayState::ENABLED
                                                         : AlgorithmDisplayState::DISABLED));
    show_algorithm_state(gctx, intrusion_channel_id, "人员入侵告警",
                         !intrusion || !intrusion->has("intrusion_alarm")
                             ? AlgorithmDisplayState::WAITING
                             : (intrusion_algorithm_enabled ? AlgorithmDisplayState::ENABLED
                                                            : AlgorithmDisplayState::DISABLED));

    if (hook_alarm && !state.last_hook_alarm)
        report_hook_alarm(gctx, hook_channel_id, crane_moving, hook);
    if (intrusion_alarm && !state.last_intrusion_alarm)
        report_intrusion_alarm(gctx, intrusion_channel_id, intrusion);
    if (helmet_alarm && !state.last_helmet_alarm)
        report_helmet_alarm(gctx, helmet_channel_id, helmet);

    state.last_hook_alarm = hook_alarm;
    state.last_intrusion_alarm = intrusion_alarm;
    state.last_helmet_alarm = helmet_alarm;
}

REGISTER_GLOBAL_LOGIC(global_crane_safety_controller);
REGISTER_GLOBAL_LOGIC_ACTION(global_crane_safety_controller,
                             global_crane_safety_controller_action);
