#pragma once
#include <atomic>
#include <gst/gst.h>
#include <mutex>
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
    int usb_width = 0;        // USB 显式采集宽度，0=自动
    int usb_height = 0;       // USB 显式采集高度，0=自动
};

struct FilePlaybackStatus
{
    bool available = false;
    bool seekable = false;
    bool playing = false;
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
};

class DecChannel;

/* GStreamer 管道元素 */
typedef struct gst_Channel
{
    DecChannel *owner;
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
    std::atomic<uint64_t> last_sample_seen_us;
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
        if (mReconnectCount.load(std::memory_order_relaxed) != 0)
            mReconnectCount.store(0, std::memory_order_relaxed);
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
    bool isFileSource() const
    {
        return mIsFileSrc;
    }
    const std::string &sourceLocation() const
    {
        return mCfg.location;
    }
    bool canShareWith(const SrcCfg_t &cfg) const;
    std::vector<int> channelIds() const
    {
        return mGstChn.chnIds;
    }
    /** 本地文件播放控制是采集层的独立能力，不依赖任何业务 Logic。 */
    bool queryFilePlayback(FilePlaybackStatus &status) const;
    bool seekFilePlayback(int64_t position_ms, int64_t &actual_position_ms, std::string &error);
    /** bus 线程释放管道前撤销公开指针，防止控制线程取得悬空对象。 */
    void detachPipeline(GstElement *pipeline);
    bool isStopRequested() const
    {
        return mStopRequested;
    }
    bool isFileEos() const
    {
        return mFileEos.load(std::memory_order_relaxed);
    }
    void setFileEos(bool value)
    {
        mFileEos.store(value, std::memory_order_relaxed);
    }
    GstChannel_t mGstChn;

  protected:
    int createVideoDecChannel(bool start_thread = true); // RTSP 管道
    int createFileDecChannel(bool start_thread = true);  // 本地文件管道
    int createUsbDecChannel(bool start_thread = true);   // USB 摄像头管道

  private:
    pthread_t mTid;
    int bObjIsInited;
    bool mThreadStarted{false}; // pipeline 可暂时离线，但监听/重连线程仍必须被 stop() join
    std::atomic<int> mReconnectCount;
    int mRecoverOkCount;
    int mRecoverFailCount;
    bool mIsFileSrc;                   // 是否为文件源
    bool mIsUsbSrc;                    // 是否为USB摄像头源
    bool mLoop;                        // 文件播放循环
    bool mStopRequested{false};        // 安全停止标志
    std::atomic<bool> mFileEos{false}; // 非循环文件已播到末尾，保留管道供 Web 回拖

    /** 只保护 pipeline 指针的发布/取引用；GStreamer 对对象操作本身是线程安全的。 */
    mutable std::mutex mPipelineMutex;

    SrcCfg_t mCfg;
};
