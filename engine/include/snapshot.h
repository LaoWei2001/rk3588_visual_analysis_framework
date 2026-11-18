/** @file snapshot.h
 * @brief 通道快照：同一发布版本的业务变量、检测结果和证据帧；媒体内部所有权对业务隐藏。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <map>
#include <memory>
#include <outputs.h>
#include <types.h>

struct ChannelPublicationMedia;

enum ChannelOnlineState
{
    CH_ONLINE = 0,  /*!< 流正常到达 */
    CH_OFFLINE = 1, /*!< 流已断开    */
};

/*================================================================
 * 通道业务快照 — 全局算法默认使用，不复制图像
 *================================================================*/
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
    bool file_paused = false; ///< 文件正在暂停或已暂停；不能作为新的实时输入参与联合判断。
    uint64_t file_timeline = 0; ///< 文件跳转或重新打开后改变，供框架重置依赖它的全局业务状态。
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

  private:
    friend struct VisionContextAccess;
    friend struct GlobalContext;
    std::shared_ptr<const ChannelPublicationMedia> media;
    /* 在采样时冻结其它全局实例的叠加层；当前实例可在本 tick 更新自己的层。 */
    std::map<std::string, std::vector<DrawCommand>> global_draw_commands_by_owner;

  public:
    /** @brief 检查已发布、在线且年龄非负的快照；max_age_ms<0 时不限制年龄。
     * 示例：
     * @code
     * ChannelLogicSnapshot snapshot;
     * bool fresh = snapshot.readable(500);
     * @endcode
     */
    bool readable(int64_t max_age_ms = -1) const
    {
        return has_publication && online_state == CH_ONLINE && publication_age_ms >= 0 &&
               (max_age_ms < 0 || publication_age_ms <= max_age_ms);
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
