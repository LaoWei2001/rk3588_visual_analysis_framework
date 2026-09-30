#include "logic/core/logic_common.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/core/utils/filesystem.hpp>
#include <opencv2/imgcodecs.hpp>

namespace
{

struct ImageWriteJob
{
    cv::Mat image;
    std::string path;
    std::string directory;
    int jpeg_quality = 95;
};

class NegativeCaptureState
{
  public:
    NegativeCaptureState() : worker_(&NegativeCaptureState::worker_loop, this) {}

    ~NegativeCaptureState()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    NegativeCaptureState(const NegativeCaptureState &) = delete;
    NegativeCaptureState &operator=(const NegativeCaptureState &) = delete;

    bool enqueue(const cv::Mat &image, std::string path, std::string directory,
                 int jpeg_quality)
    {
        if (image.empty())
            return false;
        cv::Mat owned = image.clone();
        if (owned.empty())
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || jobs_.size() >= kMaxQueuedImages)
                return false;
            jobs_.push_back({std::move(owned), std::move(path), std::move(directory),
                             jpeg_quality});
            pending_count_.fetch_add(1, std::memory_order_relaxed);
        }
        condition_.notify_one();
        return true;
    }

    uint64_t saved_count() const { return saved_count_.load(std::memory_order_relaxed); }
    uint64_t pending_count() const { return pending_count_.load(std::memory_order_relaxed); }
    uint64_t failed_count() const { return failed_count_.load(std::memory_order_relaxed); }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

    bool missing_active = false;
    uint64_t last_capture_ms = 0;
    int64_t last_enqueued_frame_id = -1;

  private:
    void set_error(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = message;
    }

    void worker_loop()
    {
        for (;;)
        {
            ImageWriteJob job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty())
                    return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            bool saved = false;
            std::string error;
            try
            {
                if (!cv::utils::fs::createDirectories(job.directory))
                {
                    error = "无法创建目录: " + job.directory;
                }
                else
                {
                    const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, job.jpeg_quality};
                    saved = cv::imwrite(job.path, job.image, params);
                    if (!saved)
                        error = "无法写入图片: " + job.path;
                }
            }
            catch (const cv::Exception &exception)
            {
                error = exception.what();
            }
            catch (const std::exception &exception)
            {
                error = exception.what();
            }

            if (saved)
            {
                saved_count_.fetch_add(1, std::memory_order_relaxed);
                set_error("");
            }
            else
            {
                failed_count_.fetch_add(1, std::memory_order_relaxed);
                set_error(error.empty() ? "未知写盘错误" : error);
            }
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static constexpr std::size_t kMaxQueuedImages = 2;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ImageWriteJob> jobs_;
    bool stopping_ = false;
    std::thread worker_;
    std::atomic<uint64_t> saved_count_{0};
    std::atomic<uint64_t> pending_count_{0};
    std::atomic<uint64_t> failed_count_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

NegativeCaptureState &capture_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<NegativeCaptureState>();
    return *std::static_pointer_cast<NegativeCaptureState>(*ctx->state);
}

int target_count(const std::vector<AlgoResult> &results, int target_class_id,
                 float min_score)
{
    int count = 0;
    for (const AlgoResult &result : results)
    {
        if (result.score < min_score)
            continue;
        if (target_class_id < 0 || result.class_id == target_class_id)
            ++count;
    }
    return count;
}

uint64_t interval_ms(float seconds)
{
    return static_cast<uint64_t>(std::llround(std::max(0.1, static_cast<double>(seconds)) * 1000.0));
}

std::string image_path(const std::string &directory, const ChannelContext *ctx,
                       const char *prefix)
{
    char filename[192];
    std::snprintf(filename, sizeof(filename), "%s_ch%02d_%llu_f%lld.jpg",
                  prefix, ctx->chnId, static_cast<unsigned long long>(ctx->unix_ms),
                  static_cast<long long>(ctx->frame_id));
    if (!directory.empty() && directory.back() == '/')
        return directory + filename;
    return directory + "/" + filename;
}

std::string resolve_output_dir(const std::string &configured)
{
    static const std::string marker = "@app_data";
    if (configured.compare(0, marker.size(), marker) != 0)
        return configured;

    std::string suffix = configured.substr(marker.size());
    if (!suffix.empty() && suffix.front() != '/')
        suffix.insert(suffix.begin(), '/');
    const char *event_store_env = std::getenv("EVENT_STORE_DIR");
    if (event_store_env && *event_store_env)
    {
        std::string event_store(event_store_env);
        while (event_store.size() > 1 && event_store.back() == '/')
            event_store.pop_back();
        const std::size_t slash = event_store.find_last_of('/');
        const std::string app_data = slash == std::string::npos
                                         ? std::string(".")
                                         : event_store.substr(0, slash);
        return app_data + suffix;
    }
    return std::string("runtime_data") + suffix;
}

void draw_status(ChannelContext *ctx, const char *text, int y,
                 const cv::Scalar &color = cv::Scalar(240, 240, 240))
{
    draw_text(ctx, text, cv::Point(18, y), color, 0.7, 1, DrawCommand::ALL,
              /*shadow_enabled=*/true, cv::Scalar(15, 45, 90), 2);
}

} // namespace

static LogicActionResult logic_training_negative_capture_action(ChannelContext *ctx,
                                                                const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "ctx or action is null"};
    if (action->name != "capture_source_frame_now")
        return {false, "unsupported action: " + action->name};

    NegativeCaptureState &state = capture_state(ctx);
    const uint64_t max_images = static_cast<uint64_t>(ctx->param_int("max_images"));
    if (state.saved_count() + state.pending_count() >= max_images)
        return {false, "已经达到图片数量上限，请增大“最多保存图片数”或重启本 Logic"};
    if (state.last_enqueued_frame_id == ctx->frame_id)
        return {false, "当前帧已经加入写盘队列，请等待下一帧"};

    const cv::Mat *source = ctx->source_frame();
    if (!source || source->empty())
        return {false, "当前视频源原始帧不可用，未保存"};
    const std::string directory = resolve_output_dir(ctx->param_string("output_dir"));
    if (directory.empty())
        return {false, "图片保存目录为空，未保存"};
    if (!state.enqueue(*source, image_path(directory, ctx, "manual"), directory,
                       static_cast<int>(ctx->param_int("jpeg_quality"))))
        return {false, "图片写盘队列正忙，请稍后重试"};

    state.last_capture_ms = ctx->timestamp_ms;
    state.last_enqueued_frame_id = ctx->frame_id;
    char message[192];
    std::snprintf(message, sizeof(message), "已加入写盘队列：原始分辨率 %dx%d，当前进度 %llu/%llu",
                  source->cols, source->rows,
                  static_cast<unsigned long long>(state.saved_count() + state.pending_count()),
                  static_cast<unsigned long long>(max_images));
    return {true, message};
}

static void logic_training_negative_capture(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    NegativeCaptureState &state = capture_state(ctx);

    const int target_class_id = static_cast<int>(ctx->param_int("target_class_id"));
    const float min_score = ctx->param_float("min_score");
    const uint64_t max_images = static_cast<uint64_t>(ctx->param_int("max_images"));
    const uint64_t saved = state.saved_count();
    const uint64_t pending = state.pending_count();
    const bool complete = saved >= max_images;
    const bool inference_ready = ctx->infer_enabled != 0;
    const int detected_count = inference_ready ? target_count(*ctx->results, target_class_id, min_score) : 0;
    const bool present = inference_ready && detected_count > 0;
    const bool capture_abnormal_count = ctx->param_bool("capture_abnormal_count");
    /* 普通模式采“0 个”；吊钩纠错模式采“不是恰好 1 个”，即 0 个或 >1 个。
     * 这样既能补纯背景，也能收集同帧多个误检框的困难样本。 */
    const bool should_capture = inference_ready &&
                                (capture_abnormal_count ? detected_count != 1 : detected_count == 0);

    bool capture_active = false;
    if (!should_capture || complete)
    {
        state.missing_active = false;
    }
    else
    {
        const bool newly_missing = !state.missing_active;
        state.missing_active = true;
        const uint64_t spacing_ms = interval_ms(ctx->param_float("capture_interval_sec"));
        const bool interval_due = newly_missing || state.last_capture_ms == 0 ||
                                  ctx->timestamp_ms - state.last_capture_ms >= spacing_ms;
        const bool quota_available = saved + pending < max_images;
        capture_active = quota_available;
        if (interval_due && quota_available && state.last_enqueued_frame_id != ctx->frame_id)
        {
            const cv::Mat *source = ctx->source_frame();
            const std::string directory = resolve_output_dir(ctx->param_string("output_dir"));
            if (source && !source->empty() && !directory.empty() &&
                state.enqueue(*source, image_path(directory, ctx, "negative"), directory,
                              static_cast<int>(ctx->param_int("jpeg_quality"))))
            {
                state.last_capture_ms = ctx->timestamp_ms;
                state.last_enqueued_frame_id = ctx->frame_id;
            }
        }
    }

    const uint64_t current_saved = state.saved_count();
    const uint64_t current_pending = state.pending_count();
    const uint64_t current_failed = state.failed_count();
    const bool current_complete = current_saved >= max_images;

    ctx->publish_bool("dataset_target_present", present);
    ctx->publish_int("dataset_target_count", detected_count);
    ctx->publish_bool("dataset_capture_active", capture_active && !current_complete);
    ctx->publish_bool("dataset_capture_complete", current_complete);
    ctx->publish_int("dataset_saved_count", static_cast<int64_t>(current_saved));
    ctx->publish_int("dataset_pending_count", static_cast<int64_t>(current_pending));
    ctx->publish_int("dataset_failed_count", static_cast<int64_t>(current_failed));
    ctx->publish_int("dataset_source_width", ctx->src_width);
    ctx->publish_int("dataset_source_height", ctx->src_height);

    char line1[224];
    char line2[224];
    char line3[256];
    char line4[320];
    const cv::Scalar green(0, 220, 0);
    const cv::Scalar orange(0, 165, 255);
    if (!inference_ready)
    {
        std::snprintf(line1, sizeof(line1), "训练集采集: 推理已关闭，暂停采集");
    }
    else if (current_complete)
    {
        std::snprintf(line1, sizeof(line1), "训练集采集: 已完成");
    }
    else if (!should_capture)
    {
        std::snprintf(line1, sizeof(line1), capture_abnormal_count
                                                     ? "训练集采集: 目标数量为1，等待数量异常"
                                                     : "训练集采集: 检测到目标，等待丢失");
    }
    else if (capture_abnormal_count)
    {
        std::snprintf(line1, sizeof(line1), "训练集采集: 目标数=%d（应为1），正在采图", detected_count);
    }
    else
    {
        std::snprintf(line1, sizeof(line1), "训练集采集: 目标丢失，正在按间隔采图");
    }
    std::snprintf(line2, sizeof(line2), "类别:%d  数量:%d  模式:%s  间隔:%.1fs",
                  target_class_id, detected_count, capture_abnormal_count ? "数量!=1" : "数量=0",
                  ctx->param_float("capture_interval_sec"));
    std::snprintf(line3, sizeof(line3), "已保存:%llu/%llu  写盘中:%llu  失败:%llu",
                  static_cast<unsigned long long>(current_saved),
                  static_cast<unsigned long long>(max_images),
                  static_cast<unsigned long long>(current_pending),
                  static_cast<unsigned long long>(current_failed));
    const std::string error = state.last_error();
    if (!error.empty())
        std::snprintf(line4, sizeof(line4), "写盘错误: %s", error.c_str());
    else
    {
        const std::string resolved_output = resolve_output_dir(ctx->param_string("output_dir"));
        std::snprintf(line4, sizeof(line4), "源分辨率:%dx%d  目录:%s",
                      ctx->src_width, ctx->src_height, resolved_output.c_str());
    }

    draw_status(ctx, line1, 32, !inference_ready || !error.empty() ? orange : green);
    draw_status(ctx, line2, 62);
    draw_status(ctx, line3, 92);
    draw_status(ctx, line4, 122, error.empty() ? cv::Scalar(240, 240, 240) : orange);
}

REGISTER_LOGIC(logic_training_negative_capture);
REGISTER_LOGIC_ACTION(logic_training_negative_capture, logic_training_negative_capture_action);
