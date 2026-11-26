/** @file runtime.h
 * @brief 独立 C++ 程序与正式视频程序共用的运行入口。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vision
{
enum class VideoChannelState
{
    Connecting,   ///< 正在连接，尚未收到当前视频源的图片。
    Streaming,    ///< 已收到图片；暂停时保持此状态，可通过 last_frame_age_ms 判断间隔。
    Disconnected, ///< 断流或文件播放结束；支持重连的视频源会继续尝试。
    Failed,       ///< 采集器启动失败，或该通道的业务函数抛出了异常；查看 error。
    Stopped       ///< run() 已完成资源清理。
};

struct VideoChannelStatus
{
    int channel_id = -1; ///< 配置中的通道 id。
    VideoChannelState state = VideoChannelState::Connecting;
    uint64_t received_frames = 0;   ///< 收到的图片总数，包含因 max_fps 限制而跳过的图片。
    int width = 0, height = 0;      ///< 最近一张原图尺寸；尚未收到时为 0。
    int64_t last_frame_age_ms = -1; ///< 距最近一次收到图片的毫秒数；尚未收到为 -1。
    std::string error;              ///< 当前错误原因；重连收到图片后清空。
    float network_rtt_ms = -1.0f;   ///< 到摄像头的 ICMP 往返毫秒数；未知或非网络源为 -1。
};

struct VideoRuntimeOptions
{
    std::string config_path;
    /** 接管 SIGINT/SIGTERM 停止、SIGPIPE 忽略；run() 结束后恢复。
     * 嵌入已有宿主时默认关闭；独立命令行程序可开启并用 Ctrl+C 退出。 */
    bool install_signal_handlers = false;
};

/** 一路文件的播放状态；暂停只影响这一路文件，摄像头不出现在此列表中。 */
struct FilePlaybackStatus
{
    int channel_id = -1;
    std::string location;
    bool available = false; ///< 文件已打开，可以接收控制。
    bool seekable = false;  ///< 支持拖动进度。
    bool playing = false;
    bool paused = false;          ///< 播放与已接收帧的分析都已经停下。
    bool pause_requested = false; ///< 用户希望暂停；busy 时可能还在处理最后几帧。
    bool busy = false;            ///< 正在执行暂停、继续或跳转。
    bool ended = false;
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
    std::string error;
};

/** 使用正式视频链路：采集/多模型推理/跟踪/业务回调/绘图/事件/录像/RTSP/数据集。
 * 在 run() 前通过 <channel.h>/<global.h> 注册业务；有业务参数时再通过 <parameters.h> 描述参数；
 * JSON 配置决定通道、模型、线程和媒体策略，无需创建 Web 业务程序包。
 *
 * 生命周期：每个对象 run() 一次；每个进程只允许一次启动尝试（失败也计入）。
 * 停止后需要重新启动程序，不支持同一进程重新初始化 GTK 等全局服务。
 * 在 run() 前已 request_stop() 的取消调用不启动资源，也不占用进程启动次数。
 * run() 阻塞到退出，并完成所有线程回收和已提交事件/录像任务的排空。
 * 开启本地显示时必须在主线程调用 run()。本类不额外创建调度线程或复制视频帧。
 * 对象必须活到 run() 返回；在另一线程运行时，调用者负责 request_stop() 后 join。
 * 不要在业务回调中销毁运行时或等待 run() 结束。
 */
class VideoRuntime
{
  public:
    /** @brief 保存启动配置；构造时不启动摄像头、推理或工作线程。
     * 示例：
     * @code
     * vision::VideoRuntimeOptions options;
     * options.config_path = "config.json";
     * vision::VideoRuntime runtime(options);
     * @endcode
     */
    explicit VideoRuntime(VideoRuntimeOptions options);
    /** @brief 释放对象；析构不代替 request_stop()+等待 run() 返回。
     * 示例：
     * @code
     * runtime.request_stop(); // 另一线程执行 run() 时，先 join 该线程再离开作用域
     * @endcode
     */
    ~VideoRuntime();
    /** @brief 禁止复制构造，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = runtime;
     * (void)borrowed;
     * @endcode
     */
    VideoRuntime(const VideoRuntime &) = delete;
    /** @brief 禁止复制赋值，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = runtime;
     * (void)borrowed;
     * @endcode
     */
    VideoRuntime &operator=(const VideoRuntime &) = delete;
    /** @brief 禁止移动构造，保证运行期间对象地址和生命周期稳定。
     * 示例：
     * @code
     * auto &borrowed = runtime;
     * (void)borrowed;
     * @endcode
     */
    VideoRuntime(VideoRuntime &&) = delete;
    /** @brief 禁止移动赋值，保证运行期间对象地址和生命周期稳定。
     * 示例：
     * @code
     * auto &borrowed = runtime;
     * (void)borrowed;
     * @endcode
     */
    VideoRuntime &operator=(VideoRuntime &&) = delete;

    /** @brief 按配置启动正式视频链路并阻塞到退出，返回前清理已启动资源。
     * @return 0 正常；1 初始化异常/NPU 忙，5 保存失败，6 重复启动，7 业务线程异常；-1~-4 为启动错误。
     * @note 业务函数抛出异常时会停止整个运行时、清理资源；可用 last_error() 查看原因。
     * @note 每进程仅一次启动尝试；本地显示开启时必须在主线程调用。
     * 示例：
     * @code
     * int exit_code = runtime.run();
     * @endcode
     */
    int run();
    /** @brief 线程安全地请求停止，不等待线程或媒体落盘；启动前调用可取消本次 run()。
     * @note 不可在 POSIX 信号处理器中调用；对象必须存活到 run() 返回。
     * 示例：
     * @code
     * runtime.request_stop();
     * @endcode
     */
    void request_stop();
    /** @brief 查询 run() 是否处于执行期；不代表所有通道健康，也不能替代线程 join。
     * 示例：
     * @code
     * bool running = runtime.is_running();
     * @endcode
     */
    bool is_running() const;

    /** @brief 查看配置中各路视频是否收到图片、是否断流，以及错误原因。
     * @return 已启用通道的状态列表；配置尚未加载时为空。运行中可从其他线程调用。
     * run() 返回后保留最后结果；正常停止的通道变为 Stopped，Failed 和错误原因保留。
     * @code
     * for (const auto &channel : runtime.channel_status())
     * {
     *     if (channel.state == vision::VideoChannelState::Failed)
     *         std::cerr << channel.channel_id << ": " << channel.error << '\n';
     * }
     * @endcode
     */
    std::vector<VideoChannelStatus> channel_status() const;

    /** 查看各路文件的位置和暂停状态。可从控制线程或业务回调中调用。
     * @code
     * for (const auto &file : runtime.file_playback())
     *     std::cout << file.channel_id << ": " << file.position_ms << " ms\n";
     * @endcode
     */
    std::vector<FilePlaybackStatus> file_playback() const;

    /** 请求暂停或继续一路文件，立即返回，不阻塞业务回调。
     * true 表示请求已接收；实际完成后 file_playback() 中 busy=false。
     * 通道不是文件、尚未就绪或已经结束时返回 false；error 可接收原因。
     * 暂停保留业务状态；继续后的 timestamp_ms、dt_ms 不包含暂停时间。
     * @code
     * std::string error;
     * if (!runtime.set_file_paused(0, true, &error))
     *     std::cerr << error << '\n';
     * // 继续时传 false。
     * @endcode
     */
    bool set_file_paused(int channel_id, bool paused, std::string *error = nullptr);

    /** 请求跳到文件的指定毫秒位置。跳转会重置该路业务状态和跟踪。
     * 暂停时跳转只分析新位置的一帧，随后保持暂停。返回值和 error 同 set_file_paused()。
     * @code
     * runtime.seek_file(0, 10000); // 跳到第 10 秒
     * @endcode
     */
    bool seek_file(int channel_id, int64_t position_ms, std::string *error = nullptr);

    /** @brief 取得导致整个运行时退出的错误；没有错误时返回空字符串。
     * 可在运行中或 run() 返回后调用；单路断流原因请看 channel_status() 中的 error。
     * @code
     * if (runtime.run() != 0)
     *     std::cerr << runtime.last_error() << '\n';
     * @endcode
     */
    std::string last_error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vision
