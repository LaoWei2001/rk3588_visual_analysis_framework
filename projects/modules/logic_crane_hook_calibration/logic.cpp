#include <channel.h>
#include <drawing.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct CalibrationState
{
    int last_track_id = -1;
    uint64_t saved_count = 0;
    int64_t last_saved_frame_id = -1;
};

struct CalibrationSample
{
    int channel_id = -1;
    int64_t frame_id = 0;
    uint64_t steady_ms = 0;
    uint64_t unix_ms = 0;
    int track_id = -1;
    int class_id = -1;
    float score = 0.0f;
    cv::Rect box;
    cv::Point center;
    double box_scale = 0.0;
};

const AlgoResult *select_hook(const std::vector<AlgoResult> *results, int class_id,
                              float min_score, int preferred_track_id)
{
    if (!results)
        return nullptr;
    const AlgoResult *same_track = nullptr;
    const AlgoResult *highest_score = nullptr;
    for (const AlgoResult &result : *results)
    {
        if (result.class_id != class_id || result.score < min_score ||
            result.box.width <= 0 || result.box.height <= 0)
            continue;
        if (preferred_track_id >= 0 && result.track_id == preferred_track_id)
            same_track = &result;
        if (!highest_score || result.score > highest_score->score)
            highest_score = &result;
    }
    return same_track ? same_track : highest_score;
}

CalibrationSample make_sample(const ChannelContext *ctx, const AlgoResult &result)
{
    CalibrationSample sample;
    sample.channel_id = ctx ? ctx->chnId : -1;
    sample.frame_id = ctx ? ctx->frame_id : 0;
    sample.steady_ms = ctx ? ctx->timestamp_ms : 0;
    sample.unix_ms = ctx ? ctx->unix_ms : 0;
    sample.track_id = result.track_id;
    sample.class_id = result.class_id;
    sample.score = result.score;
    sample.box = result.box;
    sample.center = result.box_center();
    sample.box_scale = std::sqrt(static_cast<double>(result.box.width) * result.box.height);
    return sample;
}

bool file_is_empty(const std::string &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    return !input.is_open() || input.tellg() <= 0;
}

bool append_sample_csv(const std::string &path, const CalibrationSample &sample)
{
    if (path.empty())
        return false;
    const bool write_header = file_is_empty(path);
    std::ofstream output(path, std::ios::out | std::ios::app);
    if (!output.is_open())
        return false;
    if (write_header)
    {
        output << "unix_ms,steady_ms,frame_id,channel_id,track_id,class_id,score,"
                  "box_x,box_y,box_width,box_height,center_x,center_y,box_scale\n";
    }
    output << sample.unix_ms << ',' << sample.steady_ms << ',' << sample.frame_id << ','
           << sample.channel_id << ',' << sample.track_id << ',' << sample.class_id << ','
           << std::fixed << std::setprecision(4) << sample.score << ','
           << sample.box.x << ',' << sample.box.y << ',' << sample.box.width << ','
           << sample.box.height << ',' << sample.center.x << ',' << sample.center.y << ','
           << std::setprecision(3) << sample.box_scale << '\n';
    output.flush();
    return output.good();
}

void draw_status(ChannelContext *ctx, const char *text, const cv::Point &pos,
                 const cv::Scalar &color = cv::Scalar(240, 240, 240))
{
    draw_text(ctx, text, pos, color, 0.7, 1, DrawCommand::ALL,
              /*shadow_enabled=*/true, cv::Scalar(15, 45, 90), 2);
}

CalibrationState &calibration_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<CalibrationState>();
    return *std::static_pointer_cast<CalibrationState>(*ctx->state);
}

} // namespace

static LogicActionResult logic_crane_hook_calibration_action(ChannelContext *ctx,
                                                             const LogicAction *action)
{
    if (!ctx || !ctx->state || !ctx->results || !action)
        return {false, "ctx, results or action is null"};
    if (action->name != "save_current_calibration_sample")
        return {false, "unsupported action: " + action->name};

    CalibrationState &state = calibration_state(ctx);
    const AlgoResult *hook = select_hook(ctx->results,
                                         static_cast<int>(ctx->param_int("hook_class_id")),
                                         ctx->param_float("min_score"), state.last_track_id);
    if (!hook)
        return {false, "当前帧未检测到满足置信度要求的吊钩，未保存"};

    const CalibrationSample sample = make_sample(ctx, *hook);
    const std::string output_path = ctx->param_string("output_path");
    if (!append_sample_csv(output_path, sample))
        return {false, "无法写入标定文件：" + output_path};

    state.last_track_id = hook->track_id;
    state.last_saved_frame_id = ctx->frame_id;
    ++state.saved_count;
    std::ostringstream message;
    message << "已保存样本 " << state.saved_count << "：scale="
            << std::fixed << std::setprecision(2) << sample.box_scale
            << " center=(" << sample.center.x << ',' << sample.center.y << ")";
    return {true, message.str()};
}

static void logic_crane_hook_calibration(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    CalibrationState &state = calibration_state(ctx);
    const AlgoResult *hook = select_hook(ctx->results,
                                         static_cast<int>(ctx->param_int("hook_class_id")),
                                         ctx->param_float("min_score"), state.last_track_id);

    ctx->publish_bool("calibration_hook_visible", hook != nullptr);
    ctx->publish_int("calibration_saved_count", static_cast<int64_t>(state.saved_count));

    char line1[192];
    char line2[192];
    char line3[192];
    char line4[256];
    if (!hook)
    {
        ctx->publish_number("calibration_box_scale", 0.0);
        ctx->publish_int("calibration_center_x", -1);
        ctx->publish_int("calibration_center_y", -1);
        ctx->publish_int("calibration_box_width", 0);
        ctx->publish_int("calibration_box_height", 0);
        std::snprintf(line1, sizeof(line1), "吊钩标定采集: 未检测到吊钩");
        std::snprintf(line2, sizeof(line2), "类别: %lld  最低置信度: %.2f",
                      static_cast<long long>(ctx->param_int("hook_class_id")),
                      ctx->param_float("min_score"));
        std::snprintf(line3, sizeof(line3), "已保存: %llu 条",
                      static_cast<unsigned long long>(state.saved_count));
        std::snprintf(line4, sizeof(line4), "文件: %s", ctx->param_string("output_path").c_str());
        draw_status(ctx, line1, cv::Point(18, 32), cv::Scalar(0, 165, 255));
    }
    else
    {
        state.last_track_id = hook->track_id;
        const CalibrationSample sample = make_sample(ctx, *hook);
        ctx->publish_number("calibration_box_scale", sample.box_scale);
        ctx->publish_int("calibration_center_x", sample.center.x);
        ctx->publish_int("calibration_center_y", sample.center.y);
        ctx->publish_int("calibration_box_width", sample.box.width);
        ctx->publish_int("calibration_box_height", sample.box.height);
        draw_rect(ctx, sample.box, cv::Scalar(0, 255, 255), 3);
        draw_circle(ctx, sample.center, 5, cv::Scalar(0, 255, 255), -1);
        std::snprintf(line1, sizeof(line1), "动态圆心标定: 可保存正常铅垂圆心");
        std::snprintf(line2, sizeof(line2), "框: %dx%dpx  中心: (%d,%d)",
                      sample.box.width, sample.box.height, sample.center.x, sample.center.y);
        std::snprintf(line3, sizeof(line3), "box_scale=sqrt(%d*%d)=%.2f  已保存:%llu条",
                      sample.box.width, sample.box.height, sample.box_scale,
                      static_cast<unsigned long long>(state.saved_count));
        std::snprintf(line4, sizeof(line4), "点击“保存正常铅垂圆心” -> %s",
                      ctx->param_string("output_path").c_str());
        draw_status(ctx, line1, cv::Point(18, 32), cv::Scalar(0, 220, 0));
    }
    draw_status(ctx, line2, cv::Point(18, 62));
    draw_status(ctx, line3, cv::Point(18, 92));
    draw_status(ctx, line4, cv::Point(18, 122));
}

REGISTER_LOGIC(logic_crane_hook_calibration);
REGISTER_LOGIC_ACTION(logic_crane_hook_calibration, logic_crane_hook_calibration_action);
