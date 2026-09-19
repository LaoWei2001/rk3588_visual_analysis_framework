#pragma once
#include <rkvision/channel_context.h>
enum ChannelOnlineState
{
    CH_ONLINE = 0,  /*!< 流正常到达 */
    CH_OFFLINE = 1, /*!< 流已断开    */
};

struct ChannelLogicSnapshot
{
    /* 版本与有效性：has_publication=false 表示通道尚未发布过任何业务状态。 */
    bool has_publication = false;
    bool has_frame = false;
    int channel_id = -1;
    int display_order = -1;
    uint64_t publication_seq = 0;
    uint64_t published_steady_ms = 0;

    /* 产生当前 outputs/frame 的业务帧双时钟；状态清空版本没有帧，二者为 0。 */
    uint64_t frame_steady_ms = 0;
    uint64_t frame_unix_ms = 0;
    /* 产生当前 outputs 的不可变运行配置代。 */
    uint64_t config_generation = 0;
    int64_t publication_age_ms = -1;
    int64_t logic_frame_id = 0;
    int64_t frame_seq = 0;
    int src_width = 0;
    int src_height = 0;
    bool infer_enabled = false;
    float infer_fps = 0.0f; /* 采样快照时的实时性能值，不参与 publication_seq 一致性 */
    float disp_fps = 0.0f;  /* 同上 */
    ChannelOnlineState online_state = CH_ONLINE; /*!< 快照时刻的在线状态 */
    uint64_t online_state_changed_steady_ms = 0;
    std::string logic_name;
    std::shared_ptr<const LogicOutputSet> outputs;

    /**
     * 全局 Logic 的便捷读取入口。读取失败统一返回 false：包括通道离线、尚未发布、
     * 字段不存在、类型不匹配，以及设置了 max_age_ms 后数据已过期。
     * 合法的 0、false 和空字符串仍返回 true。
     */
    bool readable(int64_t max_age_ms = -1) const
    {
        return has_publication && online_state == CH_ONLINE && publication_age_ms >= 0 &&
               (max_age_ms < 0 || publication_age_ms <= max_age_ms);
    }

    bool read_string(const char *key, std::string *out, int64_t max_age_ms = -1) const
    {
        return readable(max_age_ms) && outputs && outputs->try_get_string(key, out);
    }

    bool read_number(const char *key, double *out, int64_t max_age_ms = -1) const
    {
        return readable(max_age_ms) && outputs && outputs->try_get_number(key, out);
    }

    bool read_int(const char *key, int64_t *out, int64_t max_age_ms = -1) const
    {
        return readable(max_age_ms) && outputs && outputs->try_get_int(key, out);
    }

    bool read_bool(const char *key, bool *out, int64_t max_age_ms = -1) const
    {
        return readable(max_age_ms) && outputs && outputs->try_get_bool(key, out);
    }

    bool read_json(const char *key, std::string *out, int64_t max_age_ms = -1) const
    {
        return readable(max_age_ms) && outputs && outputs->try_get_json(key, out);
    }
};

/*================================================================
 * 通道媒体快照 — 在轻量业务快照之外深拷贝同帧图像/结果/绘制指令
 *================================================================*/
struct ChannelFrameSnapshot
{
    ChannelLogicSnapshot logic;
    cv::Mat frame;
    std::vector<AlgoResult> results;
    std::vector<RoiZone> rois;
    std::vector<DrawCommand> draw_cmds;
};


namespace rkvision_detail {
extern "C" int app_ctrl_get_channel_frame_snapshot(int channel_id, ChannelFrameSnapshot *out);
}
