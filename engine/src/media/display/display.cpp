#include "display.h"

#include "application/runtime/app_ctrl.h"
#include "application/runtime/pause_ctrl.h"
#include "application/runtime/process_signals.h"
#include "application/runtime/service_watchdog.h"
#include "common/logging.h"
#include "logic/context_access.h"
#include "vision/inference/inference_engine.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <drawing.h>
#include <gtk/gtk.h>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

bool channel_uses_depth_background(const ChannelConfig *channel)
{
    if (!channel || !channel->enable || !channel->infer_enable)
        return false;
    return std::any_of(channel->models.begin(), channel->models.end(), [](const ChannelModelConfig &model) {
        return model.enable && (model.model_type == "yolo26_depth" || model.model_type == "zip_depth") &&
               !model.model_path.empty();
    });
}

vision::OverlayOptions channel_render_params(const ChannelContext &ctx)
{
    vision::OverlayOptions p;
    const cv::Mat *frame = ctx.business_frame();
    p.chnId = ctx.chnId;
    p.inputW = frame ? frame->cols : 0;
    p.inputH = frame ? frame->rows : 0;
    p.disp_fps = ctx.disp_fps;
    p.infer_fps = ctx.infer_fps;
    p.result_frame_id = ctx.frame_id;
    p.depth_background = channel_uses_depth_background(ctx.config);
    p.inference_roi = ctx.config && ctx.config->inference_roi.has_roi() ? &ctx.config->inference_roi : nullptr;
    p.roi_zones = ctx.rois;
    p.results = ctx.results;
    p.draw_cmds = VisionContextAccess::drawing(ctx);
    return p;
}

/*======================== 显示状态 (模块级单例) ========================*/
struct DisplayState
{
    std::mutex mutex;
    char *buffer{nullptr};
    char *front{nullptr}; /* 当前可见的前缓冲区 */
    Display_t *desc{nullptr};
};
static DisplayState g_disp;

void render_runtime_overlays(cv::Mat &bgr, const vision::OverlayOptions &params, vision::OverlayCache *cache)
{
    vision::OverlayOptions effective = params;
    effective.show_fps = params.show_fps && app_ctrl_get_performance_display();
    vision::render_overlays(bgr, effective, cache);
}

void render_runtime_performance_overlay(cv::Mat &bgr, const vision::OverlayOptions &params, vision::OverlayCache *cache)
{
    vision::OverlayOptions effective = params;
    effective.show_fps = params.show_fps && app_ctrl_get_performance_display();
    vision::render_performance_overlay(bgr, effective, cache);
}

void render_channel_view(cv::Mat &bgr, int chn_id, uint64_t frame_timestamp_ms, vision::OverlayCache *performance_cache)
{
    if (bgr.empty() || !app_ctrl_has_channel(chn_id))
        return;

    auto runtime = app_ctrl_get_runtime_snapshot();
    const std::vector<RoiZone> *rois = app_ctrl_runtime_channel_rois(runtime, chn_id);
    const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, chn_id);
    if (!rois || !channel)
        return;
    std::vector<AlgoResult> results;
    std::vector<DrawCommand> commands;
    cv::Mat logic_frame;
    float disp_fps = 0.0f;
    float network_rtt_ms = -1.0f;
    int64_t result_frame_id = 0;
    uint64_t logic_ts_ms = 0;

    pthread_mutex_lock(&g_pCtrl->chn_mtx[chn_id]);
    const ChannelState &state = g_pCtrl->channels_state[chn_id];
    results = state.last_results;
    commands = state.draw_cmds;
    state.append_global_draw_commands(&commands);
    logic_frame = state.logic_display_frame;
    disp_fps = state.preview_rate.value();
    network_rtt_ms = state.network_rtt_ms;
    result_frame_id = state.published_frame_seq;
    logic_ts_ms = state.logic_display_ts_ms;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chn_id]);

    const uint64_t current_ms = frame_timestamp_ms != 0
                                    ? frame_timestamp_ms
                                    : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                std::chrono::steady_clock::now().time_since_epoch())
                                                                .count());

    if (!logic_frame.empty() && current_ms >= logic_ts_ms && current_ms - logic_ts_ms < 1000)
        cv::resize(logic_frame, bgr, bgr.size());

    vision::OverlayOptions params;
    params.chnId = chn_id;
    params.inputW = g_pCtrl->business_width;
    params.inputH = g_pCtrl->business_height;
    if (params.inputW <= 0 || params.inputH <= 0)
        return;
    params.disp_fps = pause_ctrl::is_paused() ? 0.0f : disp_fps;
    params.infer_fps = pause_ctrl::is_paused() ? 0.0f : inference_get_infer_fps(chn_id);
    params.show_network_rtt = channel->stream.src_type == "rtsp";
    params.network_rtt_ms = network_rtt_ms;
    params.result_frame_id = result_frame_id;
    params.target_mask = DrawCommand::DISPLAY;
    params.show_system_overlays = true;
    params.show_custom_overlays = true;
    params.depth_background = channel_uses_depth_background(channel);
    params.inference_roi = channel->inference_roi.has_roi() ? &channel->inference_roi : nullptr;
    params.roi_zones = rois;
    params.results = &results;
    params.draw_cmds = &commands;
    render_runtime_overlays(bgr, params, performance_cache);
}

char **dispBufferMap(Display_t *dispDesc)
{
    static Display_t stDispDesc = {0};

    if (!g_disp.front || (dispDesc->width != stDispDesc.width) || (dispDesc->height != stDispDesc.height))
    {
        if (g_disp.buffer)
        {
            free(g_disp.buffer);
            g_disp.buffer = nullptr;
        }
        memcpy(&stDispDesc, dispDesc, sizeof(Display_t));
        size_t sz = 3 * dispDesc->width * dispDesc->height;
        g_disp.buffer = (char *)malloc(sz);
        if (g_disp.buffer)
            memset(g_disp.buffer, 0, sz);
        g_disp.front = g_disp.buffer;
    }

    return &g_disp.front;
}

void dispBufferUnmap(void)
{
    std::lock_guard<std::mutex> lock(g_disp.mutex);
    free(g_disp.buffer);
    g_disp.buffer = nullptr;
    g_disp.front = nullptr;
    g_disp.desc = nullptr;
}

void display_lock()
{
    g_disp.mutex.lock();
}
void display_unlock()
{
    g_disp.mutex.unlock();
}
bool display_try_lock()
{
    return g_disp.mutex.try_lock();
}

static gboolean showWidget(GtkWidget *pImage)
{
    service_watchdog::main_heartbeat();
    if (g_pause_toggle_signal)
    {
        g_pause_toggle_signal = 0;
        pause_ctrl::toggle();
    }
    if (g_shutdown_signal)
        app_ctrl_request_stop();

    /* Ctrl+C / SIGTERM 退出检测 */
    if (g_pCtrl && !g_pCtrl->isRunning)
    {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    /* Render from front buffer, which always contains the latest completely written frame */
    char *pBuf = g_disp.front;
    if (NULL == pBuf)
    {
        return G_SOURCE_CONTINUE;
    }

    display_lock();
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data((guchar *)pBuf, GDK_COLORSPACE_RGB, FALSE, 8, g_disp.desc->width,
                                                 g_disp.desc->height, 3 * g_disp.desc->width, NULL, NULL);
    gtk_image_set_from_pixbuf(GTK_IMAGE(pImage), pixbuf);
    g_object_unref(pixbuf);
    display_unlock();

    return G_SOURCE_CONTINUE;
}
/*======================== 暂停键: GTK 键盘事件处理 ========================*/
/* 保存窗口指针和原始标题，用于在暂停/恢复时更新标题栏 */
static GtkWidget *g_main_window = nullptr;
static std::string g_win_title;

/**
 * GTK key-press-event 回调.
 * 仅在 pause_ctrl::g_enabled 为 true 时生效 (由 toggle() 内部判断).
 * 按下空格键切换暂停/运行状态，并同步更新窗口标题以提示用户。
 */
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void)widget;
    (void)user_data;
    if (event->keyval == GDK_KEY_space)
    {
        pause_ctrl::toggle();
        /* 更新窗口标题: 暂停时加 [PAUSED] 提示，恢复后还原 */
        if (g_main_window)
        {
            if (pause_ctrl::is_paused())
            {
                std::string t = g_win_title + " [PAUSED - SPACE to resume]";
                gtk_window_set_title(GTK_WINDOW(g_main_window), t.c_str());
            }
            else
            {
                gtk_window_set_title(GTK_WINDOW(g_main_window), g_win_title.c_str());
            }
        }
        return TRUE; /* 事件已处理，不再向下传递 */
    }
    return FALSE;
}

/* —— 无图形会话(SSH / VS Code Remote / systemd 服务)拉起时，也能连上板端 HDMI 的 X(:0) ——
 * 现象：这些场景下 shell 没有指向本地 :0 的 DISPLAY、也没有 XAUTHORITY cookie，gtk_init 连不上 X，
 *       于是“先得在桌面会话/命令行手动跑一次才显示”。下面在 gtk_init 之前自助补齐显示环境：
 *       DISPLAY 缺省 :0；XAUTHORITY 从正在运行的 Xorg 的 -auth 参数(或常见 cookie 路径)取。
 *       已设置的一律不动 —— 所以 ssh -X 转发、板端桌面会话、命令行里照常用各自继承的值。 */
static std::string find_xorg_auth()
{
    DIR *d = opendir("/proc");
    if (!d)
        return "";
    std::string result;
    for (struct dirent *e; (e = readdir(d)) != nullptr;)
    {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue; /* 只看 pid 目录 */
        std::string base = std::string("/proc/") + e->d_name;

        char comm[256] = {0};
        FILE *cf = fopen((base + "/comm").c_str(), "r");
        if (!cf)
            continue;
        bool got = fgets(comm, sizeof(comm), cf) != nullptr;
        fclose(cf);
        if (!got)
            continue;
        std::string name(comm);
        if (name.rfind("Xorg", 0) != 0 && name.rfind("X\n", 0) != 0)
            continue; /* 进程名为 Xorg / X */

        FILE *pf = fopen((base + "/cmdline").c_str(), "rb");
        if (!pf)
            continue;
        std::vector<std::string> args;
        std::string cur;
        for (int c; (c = fgetc(pf)) != EOF;)
        {
            if (c == 0)
            {
                if (!cur.empty())
                    args.push_back(cur);
                cur.clear();
            }
            else
                cur.push_back((char)c);
        }
        if (!cur.empty())
            args.push_back(cur);
        fclose(pf);

        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "-auth")
            {
                result = args[i + 1];
                break;
            } /* 取 -auth 后面的 cookie 路径 */
        if (!result.empty())
            break;
    }
    closedir(d);
    return result;
}

static void ensure_x_display_env()
{
    if (!getenv("DISPLAY"))
        setenv("DISPLAY", ":0", 0); /* 缺省连本地 HDMI :0(已设则不动，如 ssh -X 的转发显示) */

    if (getenv("XAUTHORITY"))
        return; /* 已有 cookie 就用现成的(桌面会话/命令行) */

    std::string auth = find_xorg_auth(); /* 1) 优先从 Xorg 的 -auth 取真实 cookie */
    if (auth.empty())
    { /* 2) 兜底常见路径 */
        const char *cands[] = {"/run/lightdm/root/:0", "/var/run/lightdm/root/:0", "/run/user/0/gdm/Xauthority",
                               "/root/.Xauthority", nullptr};
        for (int i = 0; cands[i]; ++i)
            if (access(cands[i], R_OK) == 0)
            {
                auth = cands[i];
                break;
            }
    }
    if (!auth.empty())
        setenv("XAUTHORITY", auth.c_str(), 1);
}

static GtkWidget *disp_init(const char *strWinTitle, int32_t width, int32_t height)
{
    ensure_x_display_env(); /* 必须在 gtk_init 之前：无图形会话时自助补 DISPLAY/XAUTHORITY */
    /* 板子没装无障碍(AT-SPI)总线，GTK 会刷一行 dbind-WARNING；关掉无障碍桥，纯净日志、无任何副作用 */
    setenv("NO_AT_BRIDGE", "1", 0);
    gtk_init(NULL, NULL);

    static GtkWidget *pWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (pWindow)
    {
        g_signal_connect(pWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);
        gtk_window_set_title(GTK_WINDOW(pWindow), strWinTitle);
        gtk_window_set_default_size(GTK_WINDOW(pWindow), width, height);

        /* 挂接键盘事件 — 仅在 pause_ctrl 已启用时有实际意义;
         * 即使功能关闭，连接信号也无害：toggle() 内部会直接返回。
         * gtk_widget_add_events 确保窗口接收键盘事件。 */
        gtk_widget_add_events(pWindow, GDK_KEY_PRESS_MASK);
        g_signal_connect(pWindow, "key-press-event", G_CALLBACK(on_key_press), NULL);

        /* 保存窗口指针和标题，供 on_key_press 更新标题栏 */
        g_main_window = pWindow;
        g_win_title = strWinTitle ? strWinTitle : "";
    }
    else
    {
        return NULL;
    }

    return pWindow;
}
static int32_t disp_set_loop(GtkWidget *pWindow, GSourceFunc pCamUpdate)
{
    if (NULL == pWindow)
    {
        return -1;
    }
    GtkWidget *image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(pWindow), image);
    GSource *gSource = g_timeout_source_new(33); // ~30fps refresh
    g_source_set_callback(gSource, pCamUpdate, image, NULL);
    g_source_attach(gSource, NULL);
    /* g_source_attach 内部已经为 GMainContext 增加了一份引用,
     * 调用方持有的初始引用必须释放, 否则该 GSource 永不销毁,
     * 进而 leak 一份 GLib wakeup pipe (display 热重启场景累积). */
    g_source_unref(gSource);
    return 0;
}
int display(Display_t *disp)
{
    GtkWidget *pWindow = disp_init(disp->winTitle, disp->width, disp->height);
    if (pWindow)
    {
        g_disp.desc = disp;
        disp_set_loop(pWindow, (GSourceFunc)showWidget);
        gtk_widget_show_all(pWindow);
    }
    gtk_main();

    return 0;
}
