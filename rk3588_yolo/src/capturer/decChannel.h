#pragma once
#include <gst/gst.h>
#include <stdbool.h>
#include <stdint.h>
#include <string>
#include <vector>

// 源配置
struct SrcCfg_t
{
    std::string srcType;      // "rtsp" / "file" / "usb"
    std::string location;     // RTSP URL / 本地文件路径 / USB设备节点
    std::string videoEncType; // h264/h265（仅 RTSP 有效）
    bool loop = false;        // 文件播放是否循环
};

/* GStreamer 管道元素 */
typedef struct gst_Channel
{
    std::vector<int> chnIds;
    GstElement *pipeline;
    GstElement *source;
    union {
        /* RTSP 模式 */
        struct
        {
            GstElement *h26xRTPDepay;
            GstElement *h26xParse;
        };
        /* 文件模式 */
        struct
        {
            GstElement *decoder; // decodebin
        };
    };
    GstElement *converter;  // USB: videoconvert
    GstElement *capsFilter; // USB: capsfilter(video/x-raw,format=NV12)
    GstElement *vDec;
    GstElement *vSink;
    uint64_t last_frame_time_us;
    uint64_t last_sample_seen_us;
    bool is_file;
} GstChannel_t;

class DecChannel
{
  public:
    DecChannel(int chnId, const SrcCfg_t &cfg);
    ~DecChannel();

    int init(bool start_thread = true);
    int32_t IsInited()
    {
        return bObjIsInited;
    }
    int32_t channelId() const
    {
        return mGstChn.chnIds.empty() ? -1 : mGstChn.chnIds[0];
    }
    void reconnect();
    void resetReconnectCount()
    {
        mReconnectCount = 0;
    }
    void addTargetChannel(int chnId)
    {
        mGstChn.chnIds.push_back(chnId);
    }
    /** @brief 检查此采集器是否服务指定逻辑通道 */
    bool hasChannel(int chnId) const;
    /** @brief 安全停止: 先设停止标志, 再等待 bus 线程退出, 再清理 pipeline.
     *  调用方必须确保 pipeline 已处于 NULL 状态或不再被访问。 */
    void stop();
    bool isLoop() const;
    bool isStopRequested() const
    {
        return mStopRequested;
    }
    GstChannel_t mGstChn;

  protected:
    int createVideoDecChannel(bool start_thread = true); // RTSP 管道
    int createFileDecChannel(bool start_thread = true);  // 本地文件管道
    int createUsbDecChannel(bool start_thread = true);   // USB 摄像头管道

  private:
    pthread_t mTid;
    int bObjIsInited;
    int mReconnectCount;
    int mRecoverOkCount;
    int mRecoverFailCount;
    bool mIsFileSrc;            // 是否为文件源
    bool mIsUsbSrc;             // 是否为USB摄像头源
    bool mLoop;                 // 文件播放循环
    bool mStopRequested{false}; // 安全停止标志

    SrcCfg_t mCfg;
};
