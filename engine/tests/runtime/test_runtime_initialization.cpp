// Internal lifecycle regression: no camera, model, GPIO, or network service starts.
#include "application/runtime/app_ctrl.h"
#include "pipeline/pipeline_runtime.h"
#include "vision/inference/inference_engine.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace
{
// One-shot and confined to the initializer: cleanup allocations can proceed, and
// other threads can never receive an injected exception.
thread_local int allocations_before_failure = -1;
thread_local bool allocation_failed = false;

bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "runtime initialization: %s\n", message);
    return condition;
}

bool inference_allocation_rollback()
{
    AppConfig config;
    config.enable_display = false;
    config.enable_rtsp = false;
    // Exercise configured channels. Unused slots share one immutable default filter.
    config.channels.resize(MAX_CHANNEL_NUM);
    for (int id = 0; id < MAX_CHANNEL_NUM; ++id)
        config.channels[id].id = id;
    int failure_cases = 0;
    for (int point = 0; ; ++point)
    {
        allocations_before_failure = point;
        allocation_failed = false;
        try
        {
            if (inference_init(config) != 0)
            {
                allocations_before_failure = -1;
                return require(false, "model-free channels should initialize");
            }
        }
        catch (const std::bad_alloc &)
        {
            // Partial initialization must already have released its resources.
        }
        allocations_before_failure = -1;
        const bool injected = allocation_failed;
        inference_deinit(); // Safe after rollback as well as successful startup.
        if (!injected)
            break;
        ++failure_cases;
        if (!require(inference_init(config) == 0, "inference can initialize after allocation rollback"))
            return false;
        inference_deinit();
    }
    return require(failure_cases >= MAX_CHANNEL_NUM, "fault injection covered all per-channel startup allocations");
}

bool pipeline_publication_rollback()
{
    // Configuration validation requires an existing source. This fixture is never
    // decoded: the test initializes only the control block and analysis pipeline.
    char source_path[] = "/tmp/vision-runtime-source-XXXXXX.mp4";
    const int source_fd = mkstemps(source_path, 4);
    if (!require(source_fd >= 0, "temporary source fixture created"))
        return false;
    close(source_fd);

    char path[] = "/tmp/vision-runtime-init-XXXXXX";
    const int fd = mkstemp(path);
    if (!require(fd >= 0, "temporary configuration created"))
    {
        unlink(source_path);
        return false;
    }
    char config[1024];
    const int config_size = std::snprintf(config, sizeof(config), R"json({
      "global": {"enable_display":false, "enable_rtsp":false, "tracker_enable":0},
      "channels": [{
        "id":0, "enable":true,
        "stream":{"src_type":"file", "url":"%s"},
        "models":[], "logic":"", "logic_parameters":{}
      }]
    })json",
                                          source_path);
    const bool written = config_size > 0 && static_cast<size_t>(config_size) < sizeof(config) &&
                         write(fd, config, config_size) == config_size;
    close(fd);
    const int initialized = written ? app_ctrl_init(path) : -1;
    unlink(path);
    unlink(source_path);
    if (!require(initialized == 0, "control block initialized without media startup"))
        return false;

    // First snapshot publication must fail after inference resources were ready.
    g_pCtrl->config.channels[0].logic = "missing_schema_for_rollback_test";
    const int failed = pipeline_init();
    if (!require(failed != 0, "invalid first publication rejects pipeline startup") ||
        !require(pipeline_get_logic_thread_count() == 0 && pipeline_get_display_thread_count() == 0,
                 "failed pipeline releases its queue topology"))
    {
        if (failed == 0)
        {
            pipeline_deinit();
            pipeline_destroy_display_queues();
        }
        app_ctrl_deinit();
        return false;
    }
    g_pCtrl->config.channels[0].logic.clear();
    const int restored = pipeline_init();
    const bool ok = require(restored == 0, "pipeline can initialize after publication rollback");
    if (restored == 0)
    {
        pipeline_deinit();
        pipeline_destroy_display_queues();
    }
    app_ctrl_deinit();
    return ok;
}
} // namespace

void *operator new(std::size_t size)
{
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0)
    {
        allocations_before_failure = -1;
        allocation_failed = true;
        throw std::bad_alloc();
    }
    if (void *memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept
{
    std::free(memory);
}
void *operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete[](void *memory) noexcept
{
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept
{
    std::free(memory);
}

int main()
{
    if (!inference_allocation_rollback() || !pipeline_publication_rollback())
        return 1;
    std::puts("runtime allocation/publication rollback passed");
    return 0;
}
