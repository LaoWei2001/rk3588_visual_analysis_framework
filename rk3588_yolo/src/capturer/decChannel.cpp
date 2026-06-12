#include <string>
#include <chrono>
#include <thread>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "system.h"
#include "system_opt.h"
#include "gst_opt.h"
#include "decChannel.h"
#include "../analyzer/analyzer.h"
#include "../core/app_ctrl.h"
#include "../core/pause_ctrl.h"

/* ==================== RTSP TCP 预探测 ====================
 * 在创建 mppvideodec 之前先快速确认 RTSP 端口能不能连上.
 * 网络挂时直接快速失败, 避免反复 create/destroy mppvideodec 触发
 * RK3588 内核 mpp_service session 慢漏 (一天网络抖动一次足以让多路掉帧). */
static bool probe_rtsp_tcp(const std::string &rtsp_url, int timeout_ms = 2000)
{
    /* 从 rtsp://[user:pass@]host[:port]/path 中提取 host 和 port */
    auto scheme_pos = rtsp_url.find("://");
    if (scheme_pos == std::string::npos)
        return true; /* 不是标准 URL, 跳过探测让上层走原路径 */
    std::string rest = rtsp_url.substr(scheme_pos + 3);

    auto at = rest.find('@');
    if (at != std::string::npos)
        rest = rest.substr(at + 1);

    auto slash = rest.find('/');
    if (slash != std::string::npos)
        rest = rest.substr(0, slash);

    std::string host;
    std::string port_str = "554";
    auto colon = rest.find(':');
    if (colon != std::string::npos)
    {
        host = rest.substr(0, colon);
        port_str = rest.substr(colon + 1);
    }
    else
    {
        host = rest;
    }
    if (host.empty())
        return true; /* 解析不出 host, 不拦截 */

    struct addrinfo hints{};
    struct addrinfo *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
    {
        /* DNS 解析失败 → 网络应该是挂的, 拦截这次创建 */
        return false;
    }

    bool connected = false;
    int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock >= 0)
    {
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0)
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        int ret = ::connect(sock, res->ai_addr, res->ai_addrlen);
        if (ret == 0)
        {
            connected = true;
        }
        else if (errno == EINPROGRESS)
        {
            struct pollfd pfd = {sock, POLLOUT, 0};
            int pret = ::poll(&pfd, 1, timeout_ms);
            if (pret > 0 && (pfd.revents & POLLOUT))
            {
                int so_err = 0;
                socklen_t so_len = sizeof(so_err);
                if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len) == 0 && so_err == 0)
                    connected = true;
            }
        }
        ::close(sock);
    }
    freeaddrinfo(res);
    return connected;
}

/* ==================== 回调函数 ==================== */

/* bus 消息监听使用 gst_bus_timed_pop_filtered 主动轮询,
 * 不使用 gst_bus_add_signal_watch + g_signal_connect:
 * 后者每次重连都向默认 GMainContext 累积一个 GSource（持有一对 wakeup pipe fd）
 * 而无人 detach, 多路长时间运行后必然触发 fd 耗尽。主动轮询完全等价且零泄漏。 */

/* RTSP: pad-added 连接 rtspsrc 视频 pad 到解码链路 */
static void rtsp_pad_added(GstElement *src, GstPad *new_pad, GstChannel_t *data)
{
    GstCaps *caps = gst_pad_get_current_caps(new_pad);
    if (!caps)
        return;
    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *media = gst_structure_get_string(s, "media");

    if (media && g_str_has_prefix(media, "video"))
    {
        GstPad *sink = gst_element_get_static_pad(data->h26xRTPDepay, "sink");
        if (sink && !gst_pad_is_linked(sink))
            gst_pad_link(new_pad, sink);
        if (sink)
            gst_object_unref(sink);
    }
    gst_caps_unref(caps);
}

/* 文件: decodebin pad-added 连接已解码的视频 pad 到 appsink */
static void file_pad_added(GstElement *src, GstPad *new_pad, GstChannel_t *data)
{
    GstCaps *caps = gst_pad_get_current_caps(new_pad);
    if (!caps)
        caps = gst_pad_query_caps(new_pad, NULL);
    if (!caps)
        return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(s);

    /* decodebin 的 src pad 在解码后是 video/x-raw */
    if (name && g_str_has_prefix(name, "video/x-raw"))
    {
        GstPad *sink = gst_element_get_static_pad(data->vSink, "sink");
        if (sink)
        {
            if (!gst_pad_is_linked(sink))
            {
                GstPadLinkReturn ret = gst_pad_link(new_pad, sink);
                if (ret != GST_PAD_LINK_OK)
                    g_printerr("[Ch%d] link decodebin -> appsink failed: %d\n",
                               data->chnIds.empty() ? -1 : data->chnIds[0], ret);
            }
            gst_object_unref(sink);
        }
    }
    gst_caps_unref(caps);
}

/* appsink new-sample 回调：送帧到分析管线 */
static GstFlowReturn new_sample(GstElement *sink, gpointer user_data)
{
    GstChannel_t *data = (GstChannel_t *)user_data;
    GstSample *sample;
    g_signal_emit_by_name(sink, "pull-sample", &sample);

    if (!sample)
        return GST_FLOW_OK;

    data->last_sample_seen_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    /* 收到帧说明连接成功，重置重连计数器。
     * 使用 user_data 中缓存的 GstChannel_t 指针获取 DecChannel，
     * 避免每帧调用 gst_element_get_parent() 导致的引用泄漏。 */
    DecChannel *pThis = nullptr;
    if (data->pipeline)
    {
        pThis = (DecChannel *)g_object_get_data(G_OBJECT(data->pipeline), "dec_channel_ptr");
    }
    if (pThis)
    {
        pThis->resetReconnectCount();
    }

    /* 帧率限制逻辑: drop 模式（跳帧不延迟）
     * 始终立即拉取帧以保持 GStreamer 管道畅通，
     * 若未到处理时间则直接丢弃，确保每次处理的都是最新帧。 */
    bool should_process = false;
    for (int cid : data->chnIds)
    {
        if (g_pCtrl && cid >= 0 && cid < app_ctrl_get_chn_nums())
        {
            int target_fps;
            int local_default_fps;
            {
                pthread_rwlock_rdlock(&g_pCtrl->mtx);
                target_fps = g_pCtrl->config.channels[cid].playback_fps;
                local_default_fps = g_pCtrl->config.local_default_fps;
                pthread_rwlock_unlock(&g_pCtrl->mtx);
            }
            if (target_fps <= 0 && data->is_file)
            {
                target_fps = local_default_fps;
            }

            if (target_fps > 0)
            {
                uint64_t period_us = 1000000ULL / target_fps;
                auto now = std::chrono::steady_clock::now();
                uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

                if (data->last_frame_time_us == 0) {
                    should_process = true;
                    data->last_frame_time_us = now_us;
                } else {
                    uint64_t target_time = data->last_frame_time_us + period_us;
                    if (now_us >= target_time)
                    {
                        should_process = true;
                        data->last_frame_time_us = now_us;
                        break;
                    }
                }
            } else {
                should_process = true;
                break;
            }
        }
    }
    
    if (!should_process) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    FrameDesc_t stFrameDesc;
    GstBuffer *buffer = gstopt_sample_get_buffer(sample, &stFrameDesc);
    if (!buffer)
    {
        g_printerr("[DecChannel] WARNING: gstopt_sample_get_buffer returned null, drop one frame and continue\n");
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        for (int cid : data->chnIds) {
            ImgDesc_t imgDesc = {0};
            imgDesc.chnId = cid;
            imgDesc.width = stFrameDesc.width;
            imgDesc.height = stFrameDesc.height;
            imgDesc.horStride = stFrameDesc.horStride;
            imgDesc.verStride = stFrameDesc.verStride;
            imgDesc.dataSize = map.size;
            imgDesc.fd = stFrameDesc.fd; /* DMA-BUF 零拷贝句柄 */
            snprintf(imgDesc.fmt, sizeof(imgDesc.fmt), "%s", stFrameDesc.strFmt);
            videoOutHandle((char *)map.data, imgDesc);
        }
        gst_buffer_unmap(buffer, &map);
    }
    else
    {
        g_printerr("[DecChannel] WARNING: gst_buffer_map failed, drop one frame and continue\n");
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* ==================== Bus 监听线程 ==================== */

static void *busListen(void *para)
{
    GstElement *pPipeLine = (GstElement *)para;
    if (!pPipeLine)
        pthread_exit(NULL);

    DecChannel *pThis = (DecChannel *)g_object_get_data(G_OBJECT(pPipeLine), "dec_channel_ptr");

    while (true)
    {
        /* 防御: pPipeLine 必须是有效 GstPipeline。reconnect 失败时 mGstChn.pipeline
         * 应该被置 NULL, 但这里再做一次类型检查避免 gst_element_get_bus 拿到野指针
         * 后返回 NULL bus, 进而触发 GST_IS_BUS 断言失败。*/
        if (!pPipeLine || !GST_IS_PIPELINE(pPipeLine))
        {
            g_printerr("[Ch%d] busListen: invalid pipeline pointer, exit thread\n",
                       pThis ? pThis->channelId() : -1);
            break;
        }

        GstBus *bus = gst_element_get_bus(pPipeLine);
        if (!bus)
        {
            g_printerr("[Ch%d] busListen: gst_element_get_bus returned NULL, exit thread\n",
                       pThis ? pThis->channelId() : -1);
            break;
        }
        /* bus 消息监听见文件顶部注释: 使用 gst_bus_timed_pop_filtered 主动轮询。*/

        gboolean terminate = FALSE;
        gboolean need_reconnect = FALSE;
        const char *reconnect_reason = "unknown";
        GstMessage *msg;

        do
        {
            /* 使用 500ms 超时轮询，以便检测退出信号 */
            msg = gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND,
                                             (GstMessageType)(GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_WARNING |
                                                              GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

            /* 退出检测（无论是否收到消息） */
            if ((g_pCtrl && !g_pCtrl->isRunning) || pThis->isStopRequested())
            {
                if (msg)
                    gst_message_unref(msg);
                terminate = TRUE;
                need_reconnect = FALSE;
                break;
            }

            if (!msg)
            {
                uint64_t now_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());

                /* ---- 文件源暂停/恢复: 让 GStreamer pipeline 真正停在当前位置 ----
                 *
                 * 仅对文件源 (is_file=true) 且暂停键功能已启用时生效。
                 * busListen 每 500ms 轮询一次，在此检测 pause_ctrl 状态变化
                 * 并同步 GStreamer pipeline 状态：
                 *
                 *   is_paused() && PLAYING  → 设为 GST_STATE_PAUSED
                 *     GStreamer 停在当前帧，不再解码新帧，不再触发 new_sample
                 *
                 *   !is_paused() && PAUSED  → 设为 GST_STATE_PLAYING
                 *     从断点继续，update last_sample_seen_us 防 watchdog 误判
                 *
                 * RTSP/USB 不在此处理，保持原有丢帧行为。
                 */
                if (pThis && pThis->mGstChn.is_file && pause_ctrl::g_enabled.load(std::memory_order_relaxed))
                {
                    GstState cur = GST_STATE_VOID_PENDING;
                    gst_element_get_state(pPipeLine, &cur, NULL, 0); /* 非阻塞查询 */

                    if (pause_ctrl::is_paused())
                    {
                        if (cur == GST_STATE_PLAYING)
                        {
                            gst_element_set_state(pPipeLine, GST_STATE_PAUSED);
                        }
                        /* 暂停期间持续刷新 last_sample_seen_us，防止 watchdog
                         * 误判"无新帧"并触发重连——pipeline 是我们主动暂停的，
                         * 不是卡死，不应该重连。 */
                        pThis->mGstChn.last_sample_seen_us = now_us;
                        continue;
                    }
                    else
                    {
                        if (cur == GST_STATE_PAUSED)
                        {
                            gst_element_set_state(pPipeLine, GST_STATE_PLAYING);
                            /* 恢复后重置时间戳，给 watchdog 一个干净的起点 */
                            pThis->mGstChn.last_sample_seen_us = now_us;
                        }
                    }
                }

                /* ---- 静默断流 watchdog ----
                 * 文件流或实时流在某些场景会静默停流（无 ERROR/EOS）。
                 * 通过"无新样本超时"触发通道重建，避免永久卡死。
                 * - 文件流：损坏帧导致解码器卡死，超时 3 秒
                 * - RTSP/USB：TCP 连接未断但摄像头停止推流，超时 15 秒 */
                if (pThis)
                {
                    uint64_t last_us = pThis->mGstChn.last_sample_seen_us;

                    // 文件流 3 秒无帧、实时流 15 秒无帧，均视为静默断流
                    uint64_t stall_threshold_us = pThis->mGstChn.is_file ? 3000000ULL : 15000000ULL;

                    if (last_us > 0 && now_us > last_us + stall_threshold_us)
                    {
                        const char *src_type = pThis->mGstChn.is_file ? "file" : "live";
                        g_printerr("[Ch%d] WARNING: [%s] no new sample for %.2fs (threshold %.0fs), force reconnect pipeline\n",
                                   pThis->channelId(),
                                   src_type,
                                   static_cast<double>(now_us - last_us) / 1000000.0,
                                   static_cast<double>(stall_threshold_us) / 1000000.0);
                        terminate = TRUE;
                        need_reconnect = TRUE;
                        reconnect_reason = "stall-timeout";
                        break;
                    }
                }
                continue;
            }

            switch (GST_MESSAGE_TYPE(msg))
            {
            case GST_MESSAGE_WARNING:
            {
                GError *warn;
                gchar *dbg;
                gst_message_parse_warning(msg, &warn, &dbg);
                g_printerr("[Ch%d] Warning from %s: %s\n",
                           pThis ? pThis->channelId() : -1,
                           GST_OBJECT_NAME(msg->src), warn->message);
                if (dbg)
                    g_printerr("  Debug: %s\n", dbg);
                g_clear_error(&warn);
                g_free(dbg);
                break;
            }
            case GST_MESSAGE_ERROR:
            {
                GError *err;
                gchar *dbg;
                gst_message_parse_error(msg, &err, &dbg);
                g_printerr("[Ch%d] Error from %s: %s\n",
                           pThis ? pThis->channelId() : -1,
                           GST_OBJECT_NAME(msg->src), err->message);
                if (dbg)
                    g_printerr("  Debug: %s\n", dbg);
                g_clear_error(&err);
                g_free(dbg);
                terminate = TRUE;
                need_reconnect = TRUE;
                reconnect_reason = "gst-error";
                break;
            }
            case GST_MESSAGE_EOS:
                terminate = TRUE;
                need_reconnect = TRUE;
                reconnect_reason = "gst-eos";
                break;
            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pPipeLine))
                {
                    GstState o, n, p;
                    gst_message_parse_state_changed(msg, &o, &n, &p);
                    g_print("[Ch%d] Pipeline: %s -> %s\n",
                            pThis ? pThis->channelId() : -1,
                            gst_element_state_get_name(o),
                            gst_element_state_get_name(n));
                }
                break;
            default:
                break;
            }
            gst_message_unref(msg);
        } while (!terminate);

        /* 管道清理：始终由 busListen 线程独占处理，析构函数不触碰 */
        gst_object_unref(bus);

        /* 先尝试优雅停止，超时则强制清理 */
        GstStateChangeReturn ret = gst_element_set_state(pPipeLine, GST_STATE_NULL);
        if (ret == GST_STATE_CHANGE_ASYNC)
        {
            /* 等待最多2秒，避免mppvideodec死锁时永久阻塞 */
            ret = gst_element_get_state(pPipeLine, NULL, NULL, 2 * GST_SECOND);
            if (ret == GST_STATE_CHANGE_FAILURE || ret == GST_STATE_CHANGE_ASYNC)
            {
                g_printerr("[Ch%d] Pipeline state change timeout/failed, force cleanup\n",
                           pThis ? pThis->channelId() : -1);
            }
        }

        gst_object_unref(pPipeLine);
        if (pThis)
            pThis->mGstChn.pipeline = NULL;

        /* 仅在运行中才重连 */
        if (need_reconnect && pThis && g_pCtrl && g_pCtrl->isRunning)
        {
            g_printerr("[Ch%d] Reconnect triggered, reason=%s\n",
                       pThis->channelId(), reconnect_reason);

            // 【修复点 1】本地单次播放的视频，播完立刻退出线程，绝对不重试！
            if (pThis->mGstChn.is_file && !pThis->isLoop())
            {
                g_print("[Ch%d] Local video finished, exiting bus thread safely.\n", pThis->channelId());
                /* 文件播完：通知 analyzer 逻辑通道已离线，停止推旧框/旧状态 */
                for (int cid : pThis->mGstChn.chnIds)
                    analyzer_channel_offline(cid);
                break; // 跳出外层 while(true)，彻底安全下班
            }

            /* 断流/错误：立即通知 analyzer 所有逻辑通道已离线。
             * analyzer_channel_offline 会:
             *   - 清空 last_results / draw_cmds (避免旧框冻结在画面上)
             *   - 重置跟踪器 track_id (重连后 ID 从新分配,不与旧目标混淆)
             *   - 重置 feed throttle 时间戳
             * 在所有 chnIds 上调用，因为一条 GstChannel 可能服务多个逻辑通道
             * (例如通过 addTargetChannel 共享同一 RTSP 流)。 */
            for (int cid : pThis->mGstChn.chnIds)
                analyzer_channel_offline(cid);

            // 【修复点 2】RTSP掉线等需要重连的情况，必须用 while 死等，绝不把 NULL 漏给上层！
            // 同时检测 mStopRequested，以便流地址热切换时 stop() 能快速中断此循环。
            while (g_pCtrl && g_pCtrl->isRunning && !pThis->isStopRequested())
            {
                pThis->reconnect();
                pPipeLine = pThis->mGstChn.pipeline;
                if (pPipeLine)
                {
                    /* 管道重建成功：通知 analyzer 通道恢复上线。
                     * analyzer_channel_online 会清空旧结果并复位跟踪器，
                     * 之后的第一帧推理结果将作为全新起点写入 channel_results。*/
                    for (int cid : pThis->mGstChn.chnIds)
                        analyzer_channel_online(cid);
                    break; // 成功拿到新管道，可以跳出死等
                }
                // 创建失败（比如网络断了或硬件被锁），原地睡2秒继续试，不准跑去上面！
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }

            // 如果是被 Ctrl+C 强制退出的，直接下班
            if (!pPipeLine)
            {
                break;
            }

            // 拿着健康的新管道，安心回到顶部重新监听
            continue;
        }
        break;
    } // end while (true)

    pthread_exit(NULL);
}

/* ==================== 构造 / 析构 ==================== */

DecChannel::DecChannel(int chnId, const SrcCfg_t &cfg) : bObjIsInited(false),
                                                         mReconnectCount(0),
                                                         mRecoverOkCount(0),
                                                         mRecoverFailCount(0),
                                                         mIsFileSrc(cfg.srcType == "file"),
                                                         mIsUsbSrc(cfg.srcType == "usb"),
                                                         mLoop(cfg.loop),
                                                         mCfg(cfg)
{
    mGstChn.pipeline = nullptr;
    mGstChn.source = nullptr;
    mGstChn.h26xRTPDepay = nullptr;
    mGstChn.h26xParse = nullptr;
    mGstChn.converter = nullptr;
    mGstChn.capsFilter = nullptr;
    mGstChn.vDec = nullptr;
    mGstChn.vSink = nullptr;

    mGstChn.chnIds.push_back(chnId);
    mGstChn.is_file = mIsFileSrc;
    mGstChn.last_frame_time_us = 0;
    mGstChn.last_sample_seen_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

DecChannel::~DecChannel()
{
    stop();
}

bool DecChannel::hasChannel(int chnId) const
{
    for (int id : mGstChn.chnIds)
        if (id == chnId) return true;
    return false;
}

void DecChannel::stop()
{
    if (!bObjIsInited)
        return;

    mStopRequested = true;

    /* 通知 bus 线程退出 — 设置 pipeline 为 NULL 会触发 bus 上的消息,
     * 配合 busListen 中的 isRunning / mStopRequested 检查使其快速退出。 */
    if (mGstChn.pipeline)
    {
        gst_element_set_state(mGstChn.pipeline, GST_STATE_NULL);
    }

    /* 第一轮: 给 bus 线程 3 秒优雅退出窗口 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    int ret = pthread_timedjoin_np(mTid, NULL, &ts);

    if (ret == 0)
    {
        /* 正常退出 */
        mGstChn.pipeline = NULL;
        bObjIsInited = false;
        return;
    }

    /* 第二轮: mppvideodec 偶发死锁场景, 再补一次 NULL 状态并等待 5 秒.
     * 此时 mGstChn.pipeline 可能已经被 busListen 自己置 NULL — 不再访问, 避免野指针. */
    g_printerr("[DecChannel ch%d] bus thread did not exit in 3s, retry NULL state and wait 5s more\n",
               channelId());
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    ret = pthread_timedjoin_np(mTid, NULL, &ts);

    if (ret == 0)
    {
        mGstChn.pipeline = NULL;
        bObjIsInited = false;
        return;
    }

    /* 第三轮: 仍未退出 — 极端情况(mppvideodec 内核死锁). 不 detach:
     * 调用方 stop() 只在进程退出路径调用 (见 main.cpp 步骤2), 进程退出时
     * OS 会回收所有线程及其资源, 比 detach 让线程游离继续访问 GStreamer
     * 对象更安全。这里只打印告警, 函数返回, 让 main 继续走完析构, 最终 exit. */
    g_printerr("[DecChannel ch%d] CRITICAL: bus thread still alive after 8s total wait, "
               "leaving it for process-exit reaper (no detach to avoid use-after-free)\n",
               channelId());
    mGstChn.pipeline = NULL;
    bObjIsInited = false;
}

/* ==================== 初始化 ==================== */

int DecChannel::init(bool start_thread)
{
    if (mIsFileSrc)
    {
        return createFileDecChannel(start_thread);
    }
    if (mIsUsbSrc)
    {
        return createUsbDecChannel(start_thread);
    }
    return createVideoDecChannel(start_thread);
}

/* 失败路径统一释放 pipeline 并将指针置 NULL,
 * 防止 busListen 拿到野指针调用 gst_element_get_bus 触发 GST_IS_BUS 断言。*/
static inline void release_failed_pipeline(GstChannel_t &gst_chn)
{
    if (gst_chn.pipeline)
    {
        gst_object_unref(gst_chn.pipeline);
        gst_chn.pipeline = NULL;
    }
}

int DecChannel::createVideoDecChannel(bool start_thread)
{
    /* TCP 预探测: 网络不通时直接放弃本次重建, 不创建 mppvideodec.
     * 避免网络抖动期间高频 create/destroy mppvideodec 触发 RK3588
     * 内核侧 mpp_service session 慢漏 — 这是程序运行一天后多路视频
     * 掉帧、重启进程无效、必须重启设备才能恢复的根因之一. */
    if (!probe_rtsp_tcp(mCfg.location, 2000))
    {
        g_printerr("[DecChannel ch%d] RTSP TCP probe failed for %s, skip pipeline build this round\n",
                   channelId(), mCfg.location.c_str());
        return -1;
    }

    mGstChn.pipeline = gst_pipeline_new("rtsp-pipeline");
    mGstChn.source = gst_element_factory_make("rtspsrc", "source");
    if (!mGstChn.pipeline || !mGstChn.source)
    {
        g_printerr("[DecChannel] Failed to create RTSP elements\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    if (mCfg.videoEncType == "h265")
    {
        mGstChn.h26xRTPDepay = gst_element_factory_make("rtph265depay", "h26xRTPDepay");
        mGstChn.h26xParse = gst_element_factory_make("h265parse", "h26xParse");
    }
    else
    {
        mGstChn.h26xRTPDepay = gst_element_factory_make("rtph264depay", "h26xRTPDepay");
        mGstChn.h26xParse = gst_element_factory_make("h264parse", "h26xParse");
    }
    mGstChn.vDec = gst_element_factory_make("mppvideodec", "vDec");
    mGstChn.vSink = gst_element_factory_make("appsink", "vSink");

    if (!mGstChn.h26xRTPDepay || !mGstChn.h26xParse || !mGstChn.vDec || !mGstChn.vSink)
    {
        g_printerr("[DecChannel] Failed to create video decode elements\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    g_object_set(mGstChn.vSink, "sync", FALSE, NULL);
    g_object_set(mGstChn.vSink, "emit-signals", TRUE, NULL);
    g_object_set(mGstChn.vSink, "max-buffers", 2, "drop", TRUE, NULL);
    g_signal_connect(mGstChn.vSink, "new-sample", G_CALLBACK(new_sample), &mGstChn);

    gst_bin_add_many(GST_BIN(mGstChn.pipeline),
                     mGstChn.source, mGstChn.h26xRTPDepay, mGstChn.h26xParse,
                     mGstChn.vDec, mGstChn.vSink, NULL);

    if (!gst_element_link_many(mGstChn.h26xRTPDepay, mGstChn.h26xParse,
                               mGstChn.vDec, mGstChn.vSink, NULL))
    {
        g_printerr("[DecChannel] Failed to link RTSP video elements\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    g_object_set(mGstChn.source,
                 "location", mCfg.location.c_str(),
                 "latency", 100,
                 "protocols", 0x04,
                 NULL);
    g_signal_connect(mGstChn.source, "pad-added", G_CALLBACK(rtsp_pad_added), &mGstChn);

    g_object_set_data(G_OBJECT(mGstChn.pipeline), "dec_channel_ptr", this);
    mGstChn.last_sample_seen_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    GstStateChangeReturn ret = gst_element_set_state(mGstChn.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[DecChannel] Failed to set RTSP pipeline to PLAYING\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    if (start_thread)
    {
        if (0 == CreateJoinThread(busListen, mGstChn.pipeline, &mTid))
            bObjIsInited = true;
    }
    else
    {
        bObjIsInited = true;
    }

    g_print("[DecChannel ch%d] RTSP pipeline started: %s\n", channelId(), mCfg.location.c_str());
    return bObjIsInited ? 0 : -1;
}

/* ==================== 本地文件管道 ==================== */

int DecChannel::createFileDecChannel(bool start_thread)
{
    mGstChn.pipeline = gst_pipeline_new("file-pipeline");
    mGstChn.source = gst_element_factory_make("filesrc", "source");
    mGstChn.decoder = gst_element_factory_make("decodebin", "decoder");
    mGstChn.vSink = gst_element_factory_make("appsink", "vSink");

    if (!mGstChn.pipeline || !mGstChn.source || !mGstChn.decoder || !mGstChn.vSink)
    {
        g_printerr("[DecChannel] Failed to create file pipeline elements\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    g_object_set(mGstChn.source, "location", mCfg.location.c_str(), NULL);

    /* 本地文件：sync=TRUE 按PTS播放，但使用async=FALSE避免阻塞。
     * max-buffers增加到8提供足够缓冲，drop=TRUE丢弃过旧帧。 */
    g_object_set(mGstChn.vSink, "sync", TRUE, "async", FALSE, NULL);
    g_object_set(mGstChn.vSink, "emit-signals", TRUE, NULL);
    g_object_set(mGstChn.vSink, "max-buffers", 8, "drop", TRUE, NULL);
    g_signal_connect(mGstChn.vSink, "new-sample", G_CALLBACK(new_sample), &mGstChn);

    /*
     * decodebin 内部自动创建解码器链路（qtdemux -> h264parse -> mppvideodec）。
     * 其 src pad 解码后输出 video/x-raw (NV12)，通过 pad-added 回调连接到 appsink。
     */
    g_signal_connect(mGstChn.decoder, "pad-added", G_CALLBACK(file_pad_added), &mGstChn);

    gst_bin_add_many(GST_BIN(mGstChn.pipeline),
                     mGstChn.source, mGstChn.decoder, mGstChn.vSink, NULL);

    /* 静态连接: filesrc -> decodebin */
    if (!gst_element_link(mGstChn.source, mGstChn.decoder))
    {
        g_printerr("[DecChannel] Failed to link filesrc -> decodebin\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    g_object_set_data(G_OBJECT(mGstChn.pipeline), "dec_channel_ptr", this);
    mGstChn.last_sample_seen_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    /* decodebin 需要先 PAUSED 完成 typefinding，再切 PLAYING */
    GstStateChangeReturn ret = gst_element_set_state(mGstChn.pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[DecChannel] Failed to set file pipeline to PAUSED\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    /* 等待 pipeline 达到 PAUSED 状态（decodebin 完成 typefinding） */
    ret = gst_element_get_state(mGstChn.pipeline, NULL, NULL, 5 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[DecChannel] Failed to reach PAUSED state\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    /* 切换到 PLAYING */
    ret = gst_element_set_state(mGstChn.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[DecChannel] Failed to set file pipeline to PLAYING\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    /* 等待PLAYING状态,避免某些视频卡在状态切换 */
    if (ret == GST_STATE_CHANGE_ASYNC)
    {
        ret = gst_element_get_state(mGstChn.pipeline, NULL, NULL, 3 * GST_SECOND);
        if (ret == GST_STATE_CHANGE_FAILURE || ret == GST_STATE_CHANGE_ASYNC)
        {
            g_printerr("[DecChannel] Timeout waiting for PLAYING state\n");
            release_failed_pipeline(mGstChn);
            return -1;
        }
    }

    if (start_thread)
    {
        if (0 == CreateJoinThread(busListen, mGstChn.pipeline, &mTid))
            bObjIsInited = true;
    }
    else
    {
        bObjIsInited = true;
    }

    g_print("[DecChannel ch%d] File pipeline started: %s (loop=%s)\n",
            channelId(), mCfg.location.c_str(), mLoop ? "yes" : "no");
    return bObjIsInited ? 0 : -1;
}

/* ==================== USB 摄像头管道 ==================== */

int DecChannel::createUsbDecChannel(bool start_thread)
{
    mGstChn.pipeline = gst_pipeline_new("usb-pipeline");
    mGstChn.source = gst_element_factory_make("v4l2src", "source");
    mGstChn.converter = gst_element_factory_make("videoconvert", "converter");
    mGstChn.capsFilter = gst_element_factory_make("capsfilter", "caps_filter");
    mGstChn.vSink = gst_element_factory_make("appsink", "vSink");

    if (!mGstChn.pipeline || !mGstChn.source || !mGstChn.converter ||
        !mGstChn.capsFilter || !mGstChn.vSink)
    {
        g_printerr("[DecChannel] Failed to create USB pipeline elements\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    g_object_set(mGstChn.source, "device", mCfg.location.c_str(), NULL);

    int desired_fps = 15;
    int explicit_w = 0, explicit_h = 0;
    if (g_pCtrl && channelId() >= 0 && channelId() < app_ctrl_get_chn_nums())
    {
        const ChannelConfig &ch = g_pCtrl->config.channels[channelId()];
        if (ch.playback_fps > 0)
            desired_fps = ch.playback_fps;
        else if (ch.max_fps > 0)
            desired_fps = ch.max_fps;
        else if (app_ctrl_get_max_fps() > 0)
            desired_fps = app_ctrl_get_max_fps();
        explicit_w = ch.stream.usb_width;   /* 方案B: 显式采集分辨率 */
        explicit_h = ch.stream.usb_height;
    }
    if (desired_fps < 1)
        desired_fps = 1;
    if (desired_fps > 30)
        desired_fps = 30;

    /* 某些USB摄像头（当前这只）NV12并不支持25fps离散档位，
     * 直接请求 25 会触发 not-negotiated。映射到常见离散帧率并提供后备档位。 */
    int capture_fps = 15;
    int preferred_width = 1280;
    int preferred_height = 720;
    if (explicit_w > 0 && explicit_h > 0)
    {
        /* 方案B: 显式 USB 采集分辨率(来自 config: stream.usb_width/height) ——
         * 与 ROI 抓帧用的分辨率一致、不随 max_fps 变，从而"画的区域 == 逻辑/显示拿到的区域"。
         * 帧率仍按分辨率选相机支持的离散档；推理处理帧率由 max_fps 在推理层节流，互不影响。*/
        preferred_width  = explicit_w;
        preferred_height = explicit_h;
        if (explicit_w <= 640)                            capture_fps = 30;
        else if (explicit_w <= 1280 && explicit_h <= 720) capture_fps = 15;
        else if (explicit_w <= 1280)                      capture_fps = 10;  /* 1280x960 */
        else                                              capture_fps = 5;   /* 1920x1080 */
    }
    else if (desired_fps >= 25)
    {
        capture_fps = 30;
        preferred_width = 640;
        preferred_height = 480;
    }
    else if (desired_fps >= 15)
    {
        capture_fps = 15;
        preferred_width = 1280;
        preferred_height = 720;
    }
    else if (desired_fps >= 10)
    {
        capture_fps = 10;
        preferred_width = 1280;
        preferred_height = 960;
    }
    else
    {
        capture_fps = 5;
        preferred_width = 1920;
        preferred_height = 1080;
    }

    GstCaps *preferred_caps = gst_caps_new_empty();
    gst_caps_append_structure(preferred_caps,
                              gst_structure_new("video/x-raw",
                                                "format", G_TYPE_STRING, "NV12",
                                                "width", G_TYPE_INT, preferred_width,
                                                "height", G_TYPE_INT, preferred_height,
                                                "framerate", GST_TYPE_FRACTION, capture_fps, 1,
                                                NULL));
    /* 后备档位：优先可跑起来，再由推理层 max_fps 做节流 */
    gst_caps_append_structure(preferred_caps,
                              gst_structure_new("video/x-raw",
                                                "format", G_TYPE_STRING, "NV12",
                                                "width", G_TYPE_INT, 1280,
                                                "height", G_TYPE_INT, 720,
                                                "framerate", GST_TYPE_FRACTION, 15, 1,
                                                NULL));
    gst_caps_append_structure(preferred_caps,
                              gst_structure_new("video/x-raw",
                                                "format", G_TYPE_STRING, "NV12",
                                                "width", G_TYPE_INT, 640,
                                                "height", G_TYPE_INT, 480,
                                                "framerate", GST_TYPE_FRACTION, 30, 1,
                                                NULL));
    gst_caps_append_structure(preferred_caps,
                              gst_structure_new("video/x-raw",
                                                "format", G_TYPE_STRING, "NV12",
                                                NULL));
    g_object_set(mGstChn.capsFilter, "caps", preferred_caps, NULL);
    gst_caps_unref(preferred_caps);

    g_object_set(mGstChn.vSink, "sync", FALSE, NULL);
    g_object_set(mGstChn.vSink, "emit-signals", TRUE, NULL);
    g_object_set(mGstChn.vSink, "max-buffers", 2, "drop", TRUE, NULL);
    g_signal_connect(mGstChn.vSink, "new-sample", G_CALLBACK(new_sample), &mGstChn);

    gst_bin_add_many(GST_BIN(mGstChn.pipeline),
                     mGstChn.source, mGstChn.converter, mGstChn.capsFilter, mGstChn.vSink,
                     NULL);

    if (!gst_element_link_many(mGstChn.source, mGstChn.converter, mGstChn.capsFilter, mGstChn.vSink, NULL))
    {
        g_printerr("[DecChannel] Failed to link USB pipeline elements\n");
        release_failed_pipeline(mGstChn);
        return -1;
    }

    g_object_set_data(G_OBJECT(mGstChn.pipeline), "dec_channel_ptr", this);
    mGstChn.last_sample_seen_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    g_print("[DecChannel ch%d] USB preferred caps: NV12 %dx%d @ %dfps (target infer=%dfps)\n",
            channelId(), preferred_width, preferred_height, capture_fps, desired_fps);

    GstStateChangeReturn ret = gst_element_set_state(mGstChn.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[DecChannel] Failed to set USB pipeline with preferred caps, fallback to device default\n");
        gst_element_set_state(mGstChn.pipeline, GST_STATE_NULL);

        GstCaps *fallback_caps = gst_caps_new_simple("video/x-raw",
                                                     "format", G_TYPE_STRING, "NV12",
                                                     NULL);
        g_object_set(mGstChn.capsFilter, "caps", fallback_caps, NULL);
        gst_caps_unref(fallback_caps);

        ret = gst_element_set_state(mGstChn.pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            g_printerr("[DecChannel] Failed to set USB pipeline to PLAYING\n");
            release_failed_pipeline(mGstChn);
            return -1;
        }
    }

    if (start_thread)
    {
        if (0 == CreateJoinThread(busListen, mGstChn.pipeline, &mTid))
            bObjIsInited = true;
    }
    else
    {
        bObjIsInited = true;
    }

    g_print("[DecChannel ch%d] USB pipeline started: %s\n", channelId(), mCfg.location.c_str());
    return bObjIsInited ? 0 : -1;
}

bool DecChannel::isLoop() const
{
    // 动态去全局配置里查最新的 loop 状态，实现热重载
    if (g_pCtrl && channelId() >= 0 && channelId() < app_ctrl_get_chn_nums())
    {
        return g_pCtrl->config.channels[channelId()].stream.loop;
    }
    // 如果还没初始化好，就用刚启动时的默认值
    return mLoop;
}
/* ==================== 重连 / 循环 ==================== */

void DecChannel::reconnect()
{
    /* 退出中不再重连，防止 use-after-free */
    if (!g_pCtrl || !g_pCtrl->isRunning)
        return;

    if (mIsFileSrc && mLoop)
    {
        /* 文件循环：重新创建管道，从头播放。遇到坏帧导致的静默停流时做短重试。 */
        const int kMaxRetry = 3;
        bool ok = false;
        auto t0 = std::chrono::steady_clock::now();
        for (int attempt = 1; attempt <= kMaxRetry; ++attempt)
        {
            g_print("[Ch%d] File loop: restarting from beginning (attempt %d/%d)\n",
                    channelId(), attempt, kMaxRetry);
            bObjIsInited = false;
            mGstChn.pipeline = NULL;
            /* 复用当前 bus 线程，避免重复创建监听线程导致竞态和卡死 */
            if (createFileDecChannel(false) == 0)
            {
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        auto t1 = std::chrono::steady_clock::now();
        long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (ok)
        {
            ++mRecoverOkCount;
            g_print("[Ch%d] File loop restart successful in %lldms (ok=%d, fail=%d)\n",
                    channelId(), elapsed_ms, mRecoverOkCount, mRecoverFailCount);
        }
        else
        {
            ++mRecoverFailCount;
            g_printerr("[Ch%d] File loop restart failed after %lldms (ok=%d, fail=%d)\n",
                       channelId(), elapsed_ms, mRecoverOkCount, mRecoverFailCount);
        }
        return;
    }

    if (mIsFileSrc && !mLoop)
    {
        /* 文件播放完毕，不循环 */
        g_print("[Ch%d] File playback finished\n", channelId());
        return;
    }

    /* 实时流(RTSP/USB)重连逻辑: 阶梯退避, 避免网络抖动期间高频重建.
     *
     * 原来固定 1 秒间隔的问题:
     *   30 秒的网络断流, 单路通道会 create/destroy mppvideodec 约 29 次,
     *   14 路通道一次抖动 ≈ 400 次 mppvideodec 实例生灭. RK3588 内核侧
     *   mpp_service 在这种高频场景下偶发漏 session, 整机可用解码器槽位
     *   被慢慢吃光, 表现为程序跑一天后部分通道掉帧、重启进程无效.
     *
     * 阶梯退避 + new_sample 拉到第一帧时 resetReconnectCount 自动归零,
     * 网络好时秒级恢复, 网络持续断时不会爆冲.
     *
     *   前 3 次:  1 秒  (抖动场景秒级恢复)
     *   4-8 次:   5 秒
     *   9-15 次:  15 秒
     *   16+ 次:   30 秒 (网络长断时降低风暴) */
    int delay_sec;
    if (mReconnectCount < 3)
        delay_sec = 1;
    else if (mReconnectCount < 8)
        delay_sec = 5;
    else if (mReconnectCount < 15)
        delay_sec = 15;
    else
        delay_sec = 30;

    const char *live_src_name = mIsUsbSrc ? "USB" : "RTSP";
    g_print("[Ch%d] %s reconnecting in %d seconds (attempt #%d, backoff)...\n",
            channelId(), live_src_name, delay_sec, mReconnectCount + 1);

    for (int i = 0; i < delay_sec * 10; ++i)
    {
        if (!g_pCtrl || !g_pCtrl->isRunning || mStopRequested)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mReconnectCount++;
    bObjIsInited = false;

    /* 等待旧管道清理完成 */
    int wait_count = 0;
    while (mGstChn.pipeline != NULL && wait_count < 50)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    if (mGstChn.pipeline != NULL)
    {
        g_printerr("[Ch%d] Old pipeline cleanup timeout, force reset\n", channelId());
        mGstChn.pipeline = NULL;
    }

    if (init(false) == 0)
    {
        g_print("[Ch%d] %s reconnect successful (pipeline created)\n",
                channelId(), live_src_name);
        // 不立即重置计数器，等管道真正运行后再重置
    }
    else
    {
        g_printerr("[Ch%d] %s reconnect failed, will keep retrying\n",
                   channelId(), live_src_name);
        /* 防御: 即使 create*DecChannel 内部已经 unref+置空, 这里再兜底一次,
         * 确保 busListen 拿到 mGstChn.pipeline 时不会是野指针. */
        mGstChn.pipeline = NULL;
    }
}
