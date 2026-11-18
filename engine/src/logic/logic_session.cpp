#include "context_access.h"
#include "logic_registry.h"
#include <logic_session.h>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <utility>

namespace vision
{
namespace
{
LogicParameterSet resolve_parameters(const ChannelConfig &config, const std::string &json, std::string &normalized)
{
    LogicParameterSet parameters;
    std::vector<LogicParameterError> errors;
    if (!logic_parameters_resolve(config.logic, json, &normalized, &parameters, &errors))
    {
        std::string message = "Invalid logic parameters";
        for (const auto &error : errors)
            message += "; " + error.field + ": " + error.message;
        throw std::invalid_argument(message);
    }
    return parameters;
}

void validate_image(const cv::Mat *image, bool business)
{
    if (image &&
        (image->empty() || image->type() != CV_8UC3 || (business && image->size() != business_coordinates::size())))
        throw std::invalid_argument(business ? "Business frame must be 640x640 CV_8UC3 BGR"
                                             : "Source frame must be nonempty CV_8UC3 BGR");
}
} // namespace

struct LogicSession::Impl : EventSink
{
    ChannelConfig config;
    ChannelLogicFunc logic;
    ChannelLogicActionFunc action;
    ChannelContext context{};
    LogicParameterSet parameters;
    LogicOutputSet outputs;
    std::shared_ptr<void> state;
    std::vector<RoiZone> rois;
    std::vector<DrawCommand> drawing;
    std::vector<LogicEvent> recorded_events;
    LogicFrame input;
    cv::Mat resized_business;
    bool business_ready = false;
    cv::Mat canvas;
    bool show_canvas = false;
    bool has_frame = false;

    explicit Impl(const LogicSessionOptions &options)
        : config(options.channel), logic(options.logic ? options.logic : channel_logic_get(config.logic.c_str())),
          action(options.action ? options.action : channel_logic_action_get(config.logic.c_str())),
          rois(make_roi_zones(config.roi_zones))
    {
        if (!logic)
            throw std::invalid_argument("LogicSession requires a callback or registered channel.logic");
        std::string normalized;
        parameters = resolve_parameters(config, config.logic_parameters_json, normalized);
        config.logic_parameters_json = std::move(normalized);
        context.chnId = config.id;
        context.config = &config;
        context.state = &state;
        context.infer_enabled = config.infer_enable;
        VisionContextAccess::bind_parameters(context, &parameters);
        VisionContextAccess::bind_outputs(context, &outputs);
        VisionContextAccess::bind_drawing(context, &drawing);
        VisionContextAccess::bind_canvas(context, &canvas, &show_canvas);
        VisionContextAccess::bind_events(context, options.events ? options.events : this);
        VisionContextAccess::bind_frames(context, business_frame, source_frame, this);
    }

    static const cv::Mat *source_frame(void *opaque)
    {
        return static_cast<Impl *>(opaque)->input.source;
    }

    static const cv::Mat *business_frame(void *opaque)
    {
        Impl &self = *static_cast<Impl *>(opaque);
        if (self.input.business)
            return self.input.business;
        if (!self.input.source)
            return nullptr;
        if (self.input.source->size() == business_coordinates::size())
            return self.input.source;
        if (!self.business_ready)
        {
            cv::resize(*self.input.source, self.resized_business, business_coordinates::size(), 0, 0, cv::INTER_LINEAR);
            self.business_ready = true;
        }
        return &self.resized_business;
    }

    EventReportResult report(ChannelContext &ctx, const EventRequest &request) override
    {
        LogicEvent event;
        event.frame_id = ctx.frame_id;
        event.timestamp_ms = ctx.timestamp_ms;
        event.request = request;
        event.drawing = drawing;
        recorded_events.push_back(std::move(event));
        EventReportResult result;
        result.status = EventReportStatus::CREATED;
        result.event_id = "replay-" + std::to_string(ctx.frame_id) + "-" + std::to_string(recorded_events.size());
        result.detail = "Captured by LogicSession; no persistence or delivery";
        return result;
    }
};

LogicSession::LogicSession(const LogicSessionOptions &options) : impl_(new Impl(options))
{
}
LogicSession::~LogicSession() = default;

void LogicSession::run(const LogicFrame &frame)
{
    validate_image(frame.source, false);
    validate_image(frame.business, true);
    Impl &self = *impl_;
    // 回放也可故意提供倒退时间，验证业务防御逻辑；dt 不允许 unsigned 下溢。
    const uint64_t analysis_time = VisionContextAccess::analysis_time(frame.timestamp_ms, frame.video_time_ms);
    const bool timeline_changed =
        self.has_frame && ((frame.video_time_ms >= 0) != (self.input.video_time_ms >= 0) ||
                           (frame.video_time_ms >= 0 && frame.video_time_ms < self.input.video_time_ms));
    if (timeline_changed)
        self.state.reset();
    self.context.dt_ms = self.has_frame && !timeline_changed && analysis_time >= self.context.timestamp_ms
                             ? static_cast<float>(analysis_time - self.context.timestamp_ms)
                             : 0.0f;
    self.context.frame_id = frame.frame_id >= 0 ? frame.frame_id : self.context.frame_id + 1;
    self.context.timestamp_ms = analysis_time;
    self.context.real_time_ms = frame.timestamp_ms;
    self.context.unix_ms = frame.unix_ms;
    self.context.video_time_ms = frame.video_time_ms;
    self.context.results = frame.results;
    if (frame.video_time_ms >= 0 && frame.results)
        for (auto &result : *frame.results)
            result.timestamp_ms = analysis_time;
    self.context.rois = frame.rois ? frame.rois : &self.rois;
    self.context.inference_valid = frame.inference_valid;
    self.context.infer_enabled = self.config.infer_enable && frame.inference_enabled;
    const cv::Mat *source = frame.source ? frame.source : frame.business;
    self.context.src_width = source ? source->cols : 0;
    self.context.src_height = source ? source->rows : 0;
    self.input = frame;
    self.business_ready = false;
    self.canvas.release();
    self.show_canvas = false;
    self.outputs = LogicOutputSet{};
    self.drawing.clear();
    self.recorded_events.clear();
    self.has_frame = true;
    self.logic(&self.context);
}

LogicActionResult LogicSession::dispatch(const LogicAction &action)
{
    if (!impl_->has_frame)
        throw std::logic_error("Run a frame before dispatching an action");
    if (!action.logic_name.empty() && action.logic_name != impl_->config.logic)
    {
        LogicActionResult result;
        result.message = "Action belongs to another logic module";
        return result;
    }
    return impl_->action ? impl_->action(&impl_->context, &action) : LogicActionResult{};
}

LogicReloadImpact LogicSession::set_parameters(const std::string &json)
{
    std::string normalized;
    LogicParameterSet parameters = resolve_parameters(impl_->config, json, normalized);
    const auto impact =
        logic_parameters_reload_impact(impl_->config.logic, impl_->config.logic_parameters_json, normalized);
    if (impact == LogicReloadImpact::RESTART_REQUIRED)
        throw std::invalid_argument("Changed parameters require constructing a new LogicSession");
    impl_->parameters = std::move(parameters);
    impl_->config.logic_parameters_json = std::move(normalized);
    if (impact == LogicReloadImpact::RESET_STATE)
        reset_state();
    return impact;
}

void LogicSession::reset_state()
{
    impl_->state.reset();
}
const ChannelContext &LogicSession::context() const
{
    return impl_->context;
}
const LogicOutputSet &LogicSession::outputs() const
{
    return impl_->outputs;
}
const std::vector<DrawCommand> &LogicSession::drawing() const
{
    return impl_->drawing;
}
const std::vector<LogicEvent> &LogicSession::events() const
{
    return impl_->recorded_events;
}
const cv::Mat *LogicSession::display_frame() const
{
    return impl_->show_canvas ? &impl_->canvas : nullptr;
}
} // namespace vision
