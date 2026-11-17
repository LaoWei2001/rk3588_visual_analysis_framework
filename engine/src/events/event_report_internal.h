#pragma once

// Private event pipeline contracts. Queues and ownership remain shared by every stage.
#include "event_report.h"
#include "media/display/display.h"
#include "media/recorder/event_video_recorder.h"
#include <atomic>
#include <json.h>
#include <map>
#include <pthread.h>
#include <queue>
#include <vector>

namespace event_report_detail
{
enum EventMedia : uint32_t
{
    EVENT_MEDIA_NONE = 0,
    EVENT_MEDIA_IMAGE = 1u << 0,
    EVENT_MEDIA_VIDEO = 1u << 1
};

struct ImageJob
{
    struct Pane
    {
        int channel_id = -1;
        uint64_t publication_seq = 0;
        int64_t frame_id = 0;
        uint64_t frame_unix_ms = 0;
        cv::Mat frame;
        vision::OverlayOptions render_params;
        InferenceRoiConfig inference_roi;
        std::vector<RoiZone> rois;
        std::vector<AlgoResult> results;
        std::vector<DrawCommand> commands;
    };

    std::string event_dir;
    cv::Mat annotated;
    cv::Mat raw;
    bool need_annotated = false;
    bool need_raw = false;
    std::string overlay_mode;
    vision::OverlayOptions render_params;
    InferenceRoiConfig inference_roi;
    std::vector<RoiZone> rois;
    std::vector<AlgoResult> results;
    std::vector<DrawCommand> commands;
    int composite_width = 0;
    int composite_height = 0;
    int composite_rows = 0;
    int composite_cols = 0;
    std::vector<Pane> panes;
};

struct CompositeImage
{
    int width = 0;
    int height = 0;
    int rows = 0;
    int cols = 0;
    std::string selection_mode;
    std::vector<int> requested_channel_ids;
    std::vector<int> missing_channel_ids;
    std::vector<ImageJob::Pane> panes;
};

struct ActiveEvent
{
    std::string event_id;
    uint64_t last_trigger_ms = 0;
};

struct PersistenceJob
{
    enum Type
    {
        CREATE,
        MERGE
    } type = CREATE;

    std::string event_id;
    std::string event_dir;
    std::string merge_key;
    std::string delivery_json;
    std::string media_json;
    std::string event_json;
    bool has_image_job = false;
    ImageJob image_job;
    bool has_video_job = false;
    EventVideoRequest video_job;

    uint64_t trigger_unix_ms = 0;
    std::string trigger_time;
    EventFields fields;
    bool extend_video = false;
};

struct DeliveryRequirements
{
    int count = 0;
    uint32_t media_flags = EVENT_MEDIA_NONE;
    bool annotated_image = false;
    bool raw_image = false;
};

struct ImageSelectionPolicy
{
    std::string mode = "connected";
    std::vector<int> channel_ids;
};

extern pthread_mutex_t g_mtx;
extern pthread_cond_t g_cv;
extern bool g_running;
extern std::queue<ImageJob> g_image_jobs;
extern pthread_mutex_t g_persist_mtx;
extern pthread_cond_t g_persist_cv;
extern std::queue<PersistenceJob> g_persist_jobs;
extern std::map<std::string, ActiveEvent> g_active;

void record_failure(const char *operation, const std::string &path);
uint64_t steady_now_ms();
int mkdir_p(const std::string &path);
std::string store_dir_path();
std::string store_dir();
void remove_tree(const std::string &dir);
void enforce_outbox_cap();
std::string make_event_id(const std::string &scope);
cJSON *fields_to_json(const EventFields &fields);
cJSON *parse_object_or_empty(const std::string &text);
DeliveryRequirements delivery_requirements(cJSON *policy, const std::string &event_type);
void add_delivery_statuses(cJSON *policy, const std::string &event_type, cJSON *root);
bool json_to_text(cJSON *root, std::string &out);
bool atomic_write_text(const std::string &path, const std::string &text);
bool record_merged_trigger(const std::string &event_id, uint64_t trigger_unix_ms, const std::string &trigger_time,
                           const EventFields &fields);
void clear_active_event(const std::string &merge_key, const std::string &event_id);
void clear_active_event_by_id(const std::string &event_id);
void refresh_media_status(cJSON *root);
bool update_media_state(const std::string &event_dir, const char *kind, const char *status, const char *error,
                        std::initializer_list<std::pair<const char *, const char *>> files = {});
void *image_worker(void *);
bool ensure_workers_locked(bool need_image_worker);
double policy_number(cJSON *policy, const char *key, double fallback);
std::string policy_string(cJSON *policy, const char *key, const char *fallback);
void append_unique_channel(std::vector<int> &channels, int channel_id);
cJSON *channel_ids_to_json(const std::vector<int> &channels);
bool parse_image_selection(cJSON *policy, ImageSelectionPolicy *out, std::string *error);
} // namespace event_report_detail
