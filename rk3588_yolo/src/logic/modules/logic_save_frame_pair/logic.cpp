#include "logic/core/logic_common.h"

#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

struct SaveFramePairState
{
    bool saved = false;
    uint64_t capture_unix_ms = 0;
    uint64_t last_failure_log_ms = 0;
};

SaveFramePairState &save_frame_pair_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<SaveFramePairState>();
    return *std::static_pointer_cast<SaveFramePairState>(*ctx->state);
}

bool directory_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensure_directory(const std::string &path)
{
    if (path.empty()) return false;

    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i != path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (part.empty() || directory_exists(part)) continue;
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
        if (!directory_exists(part)) return false;
    }
    return directory_exists(path);
}

std::string join_path(const std::string &directory, const std::string &name)
{
    if (!directory.empty() && directory.back() == '/') return directory + name;
    return directory + "/" + name;
}

void log_failure_throttled(ChannelContext *ctx, SaveFramePairState &state, const char *message)
{
    const uint64_t now_ms = ctx->timestamp_ms;
    if (state.last_failure_log_ms != 0 && now_ms - state.last_failure_log_ms < 5000ULL)
        return;
    fprintf(stderr, "[logic_save_frame_pair][ch%02d] %s\n", ctx->chnId, message);
    state.last_failure_log_ms = now_ms;
}

} // namespace

static void logic_save_frame_pair(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame || ctx->frame->empty())
        return;

    SaveFramePairState &state = save_frame_pair_state(ctx);
    if (state.saved) return;

    /* 原始帧入口仅在同步传统 CV 通道保证可用。未就绪时保留未完成状态，下一帧重试。 */
    const cv::Mat *source = ctx->source_frame();
    if (!source || source->empty())
    {
        log_failure_throttled(ctx, state,
            "source_frame() unavailable; configure this channel as a non-inference traditional-CV channel");
        return;
    }

    const std::string output_dir = ctx->param_string("output_dir");
    if (!ensure_directory(output_dir))
    {
        log_failure_throttled(ctx, state, "cannot create or access output_dir");
        return;
    }

    if (state.capture_unix_ms == 0)
        state.capture_unix_ms = ctx->unix_ms;

    char base[96];
    snprintf(base, sizeof(base), "ch%02d_%llu",
             ctx->chnId, static_cast<unsigned long long>(state.capture_unix_ms));

    const std::string source_path = join_path(output_dir, std::string(base) + "_source.jpg");
    const std::string model_path = join_path(output_dir, std::string(base) + "_model.jpg");
    const std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, 95};

    bool source_ok = false;
    bool model_ok = false;
    try
    {
        source_ok = cv::imwrite(source_path, *source, jpeg_params);
        model_ok = cv::imwrite(model_path, *ctx->frame, jpeg_params);
    }
    catch (const cv::Exception &e)
    {
        fprintf(stderr, "[logic_save_frame_pair][ch%02d] imwrite exception: %s\n",
                ctx->chnId, e.what());
        return;
    }

    if (!source_ok || !model_ok)
    {
        log_failure_throttled(ctx, state, "failed to save one or both frame images");
        return;
    }

    state.saved = true;
    state.last_failure_log_ms = 0;
    printf("[logic_save_frame_pair][ch%02d] saved source frame: %s (%dx%d)\n",
           ctx->chnId, source_path.c_str(), source->cols, source->rows);
    printf("[logic_save_frame_pair][ch%02d] saved model frame:  %s (%dx%d)\n",
           ctx->chnId, model_path.c_str(), ctx->frame->cols, ctx->frame->rows);
}

REGISTER_LOGIC(logic_save_frame_pair);
