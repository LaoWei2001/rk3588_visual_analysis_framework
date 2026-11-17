#include "event_report_internal.h"
#include "event_storage.h"
#include <algorithm>
#include <opencv2/imgcodecs.hpp>
#include <utility>

namespace event_report_detail
{
cv::Rect composite_tile_rect(const ImageJob &job, size_t pane_index)
{
    if (job.composite_width <= 0 || job.composite_height <= 0 || job.composite_cols <= 0 || job.composite_rows <= 0)
        return cv::Rect();

    const int index = static_cast<int>(pane_index);
    const int col = index % job.composite_cols;
    const int row = index / job.composite_cols;
    if (row >= job.composite_rows)
        return cv::Rect();

    /* 与实时显示窗格使用相同的 4 像素宽 / 2 像素高边界对齐规则。这样即使显示宽高
     * 不能被行列数整除，上报图中每个窗格的位置和尺寸也仍与 HDMI/RTSP 一致。 */
    const int grid_width = job.composite_width / job.composite_cols;
    const int grid_height = job.composite_height / job.composite_rows;
    const int left = (col * grid_width) & ~3;
    const int top = (row * grid_height) & ~1;
    const int right = col == job.composite_cols - 1 ? job.composite_width & ~3 : ((col + 1) * grid_width) & ~3;
    const int bottom = row == job.composite_rows - 1 ? job.composite_height & ~1 : ((row + 1) * grid_height) & ~1;
    return right > left && bottom > top ? cv::Rect(left, top, right - left, bottom - top) : cv::Rect();
}

void render_composite_image(ImageJob &job)
{
    if (job.panes.empty())
        return;

    if (job.need_raw)
        job.raw = cv::Mat::zeros(job.composite_height, job.composite_width, CV_8UC3);
    if (job.need_annotated)
        job.annotated = cv::Mat::zeros(job.composite_height, job.composite_width, CV_8UC3);

    for (size_t index = 0; index < job.panes.size(); ++index)
    {
        ImageJob::Pane &pane = job.panes[index];
        const cv::Rect tile = composite_tile_rect(job, index);
        if (pane.frame.empty() || tile.empty())
            continue; /* 与实时拼接一致：暂时无帧的已选通道保留黑色窗格。 */

        cv::Mat resized;
        cv::resize(pane.frame, resized, tile.size());
        if (job.need_raw)
            resized.copyTo(job.raw(tile));
        if (!job.need_annotated)
            continue;

        cv::Mat annotated_tile = job.annotated(tile);
        resized.copyTo(annotated_tile);
        if (job.overlay_mode == "none")
            continue;

        pane.render_params.inference_roi = pane.inference_roi.has_roi() ? &pane.inference_roi : nullptr;
        pane.render_params.roi_zones = &pane.rois;
        pane.render_params.results = &pane.results;
        pane.render_params.draw_cmds = &pane.commands;
        render_runtime_overlays(annotated_tile, pane.render_params);
    }
}

void *image_worker(void *)
{
    while (true)
    {
        ImageJob job;
        pthread_mutex_lock(&g_mtx);
        while (g_image_jobs.empty() && g_running)
            pthread_cond_wait(&g_cv, &g_mtx);
        if (g_image_jobs.empty() && !g_running)
        {
            pthread_mutex_unlock(&g_mtx);
            break;
        }
        job = std::move(g_image_jobs.front());
        g_image_jobs.pop();
        pthread_mutex_unlock(&g_mtx);

        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        bool snapshot_ok = false;
        bool raw_ok = false;
        std::string write_error;
        try
        {
            if (!job.panes.empty())
            {
                render_composite_image(job);
            }
            else if (job.need_annotated)
            {
                /* report_event 只传递不可变 Mat 引用；深拷贝和叠加统一留在图片线程。 */
                job.annotated = job.overlay_mode == "none" ? job.raw : job.raw.clone();
                if (job.overlay_mode != "none" && !job.annotated.empty())
                {
                    job.render_params.inference_roi = job.inference_roi.has_roi() ? &job.inference_roi : nullptr;
                    job.render_params.roi_zones = &job.rois;
                    job.render_params.results = &job.results;
                    job.render_params.draw_cmds = &job.commands;
                    render_runtime_overlays(job.annotated, job.render_params);
                }
            }
            snapshot_ok =
                !job.need_annotated ||
                (!job.annotated.empty() && cv::imwrite(job.event_dir + "/annotated.jpg", job.annotated, params) &&
                 event_storage::sync_file(job.event_dir + "/annotated.jpg"));
            raw_ok = !job.need_raw || (!job.raw.empty() && cv::imwrite(job.event_dir + "/raw.jpg", job.raw, params) &&
                                       event_storage::sync_file(job.event_dir + "/raw.jpg"));
        }
        catch (const cv::Exception &exc)
        {
            write_error = exc.what();
        }
        if (snapshot_ok && raw_ok)
        {
            if (job.need_annotated && job.need_raw)
                update_media_state(job.event_dir, "image", "ready", "",
                                   {{"annotated_image", "annotated.jpg"}, {"raw_image", "raw.jpg"}});
            else if (job.need_annotated)
                update_media_state(job.event_dir, "image", "ready", "", {{"annotated_image", "annotated.jpg"}});
            else
                update_media_state(job.event_dir, "image", "ready", "", {{"raw_image", "raw.jpg"}});
        }
        else
        {
            const char *fallback = !snapshot_ok && !raw_ok ? "snapshot and raw image write failed"
                                   : !snapshot_ok          ? "snapshot image write failed"
                                                           : "raw image write failed";
            const char *reason = write_error.empty() ? fallback : write_error.c_str();
            record_failure(reason, job.event_dir);
            if (snapshot_ok)
                update_media_state(job.event_dir, "image", "failed", reason, {{"annotated_image", "annotated.jpg"}});
            else if (raw_ok)
                update_media_state(job.event_dir, "image", "failed", reason, {{"raw_image", "raw.jpg"}});
            else
                update_media_state(job.event_dir, "image", "failed", reason);
            const size_t slash = job.event_dir.find_last_of('/');
            clear_active_event_by_id(slash == std::string::npos ? job.event_dir : job.event_dir.substr(slash + 1));
        }
        enforce_outbox_cap();
    }
    return nullptr;
}

} // namespace event_report_detail
