/** @file channel_context.cpp 通道上下文的图片访问、参数读取和业务变量发布。 */
#include <channel.h>
#include <ctime>
#include <opencv2/imgproc.hpp>
#include <parameters.h>
#include <stdexcept>
#include <utility>

const cv::Mat *ChannelContext::business_frame() const
{
    return business_frame_getter ? business_frame_getter(frame_getter_opaque) : nullptr;
}

const cv::Mat *ChannelContext::source_frame() const
{
    return source_frame_getter ? source_frame_getter(frame_getter_opaque) : nullptr;
}

bool ChannelContext::replace_display_frame(cv::Mat frame)
{
    if (!canvas || !show_canvas || frame.empty() || frame.depth() != CV_8U)
        return false;

    if (frame.channels() == 3)
    {
        /* cv::Mat 按引用计数持有像素；这里移动 Mat 头，不深拷贝整帧。 */
        *canvas = std::move(frame);
    }
    else if (frame.channels() == 1)
    {
        cv::cvtColor(frame, *canvas, cv::COLOR_GRAY2BGR);
    }
    else if (frame.channels() == 4)
    {
        cv::cvtColor(frame, *canvas, cv::COLOR_BGRA2BGR);
    }
    else
    {
        return false;
    }

    *show_canvas = !canvas->empty();
    return *show_canvas;
}

/* 取可写显示画布: 首次调用以当前帧为底克隆一张可写副本，并标记"本帧用它当显示底图"。
 * 之后随意 cv:: 处理(滤镜/贴图/putText 等)；中文叠加用 draw_text(走 draw_cmds, 会叠在它上面)。 */
cv::Mat &ChannelContext::display_canvas()
{
    if (!canvas || !show_canvas)
        throw std::logic_error("display_canvas requires a bound LogicSession or runtime context");
    const cv::Mat *frame = business_frame();
    if (canvas->empty() && frame && !frame->empty())
        *canvas = frame->clone();
    *show_canvas = true;
    return *canvas;
}

/* unix_ms(epoch 毫秒)→ 本地时区时间串。localtime_r 线程安全(logic 在 worker 线程跑)。 */
std::string ChannelContext::time_str() const
{
    time_t sec = static_cast<time_t>(unix_ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

/*======================== 模块专有参数访问 ========================*/
bool ChannelContext::has_param(const char *key) const
{
    return logic_parameters && logic_parameters->has(key);
}

float ChannelContext::param_float(const char *key) const
{
    return logic_parameters ? logic_parameters->get_float(key) : 0.0f;
}

int64_t ChannelContext::param_int(const char *key) const
{
    return logic_parameters ? logic_parameters->get_int(key) : 0;
}

bool ChannelContext::param_bool(const char *key) const
{
    return logic_parameters ? logic_parameters->get_bool(key) : false;
}

std::string ChannelContext::param_string(const char *key) const
{
    return logic_parameters ? logic_parameters->get_string(key) : std::string();
}

std::string ChannelContext::param_json(const char *key) const
{
    return logic_parameters ? logic_parameters->get_json(key) : std::string();
}

void ChannelContext::publish_string(const char *key, const std::string &value) const
{
    if (outputs)
        outputs->set_string(key, value);
}

void ChannelContext::publish_number(const char *key, double value) const
{
    if (outputs)
        outputs->set_number(key, value);
}

void ChannelContext::publish_int(const char *key, int64_t value) const
{
    if (outputs)
        outputs->set_int(key, value);
}

void ChannelContext::publish_bool(const char *key, bool value) const
{
    if (outputs)
        outputs->set_bool(key, value);
}

void ChannelContext::publish_json(const char *key, const std::string &json) const
{
    if (outputs)
        outputs->set_json(key, json);
}
