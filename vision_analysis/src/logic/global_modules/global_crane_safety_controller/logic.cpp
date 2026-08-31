#include "event/event_report.h"
#include "logic/core/global_logic.h"
#include "logic/global_modules/global_crane_safety_controller/safety_controller.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace
{

struct CraneSafetyState
{
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
    bool helmet_alarm = !crane_moving && scene_ready && !manual_lamp_override && raw_helmet_alarm;
    bool intrusion_alarm = crane_moving && scene_ready && raw_intrusion_alarm;
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
        helmet_alarm = false;
        intrusion_alarm = false;
        state.automatic_horn_on = !state.outputs_forced_off && !state.horn_silenced && hook_alarm;
        horn_on = state.automatic_horn_on || horn_test_active;
        state.hardware.apply(hardware_config, lamp_on, horn_on);
    }

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
