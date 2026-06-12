/**
 * @file logic_dify_person_verify.cpp
 * @brief logic_dify_person_verify —— 人员检测 Dify 二次核验。
 *
 * 功能: 当画面中出现未上报过的人员时, 上报至 Dify 进行二次核验。
 * 利用 tracker 分配的 track_id 防止对同一人员的重复报警。
 *
 * 去重策略:
 *   - 每帧收集当前画面中所有 person 的 track_id (需 track_id >= 0).
 *   - 新出现的 track_id 触发 Dify 上报; 已在 reported_ids 中的跳过.
 *   - track_id 连续丢失 MISS_FRAME_MAX 帧后才从 reported_ids 清除 (容忍短暂遮挡/漏检).
 *   - 两次上报之间至少间隔 MIN_INTERVAL_MS.
 *
 * 跨帧状态见 logic_tools.h 的 DifyPersonVerifyState。
 */
#include "logic_common.h"

static void logic_dify_person_verify(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;
    if (!ctx->frame || ctx->frame->empty())
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<DifyPersonVerifyState>();
    auto &s = *std::static_pointer_cast<DifyPersonVerifyState>(*ctx->state);

    static constexpr uint64_t MIN_INTERVAL_MS = 5000;
    static constexpr int MISS_FRAME_MAX = 30;         // 连续丢帧超过此数才清除 track_id
    static constexpr uint64_t DBG_INTERVAL_MS = 2000; // 调试打印间隔

    if (s.first)
    {
        s.last_upload_ms = ctx->timestamp_ms;
        s.last_dbg_ms = ctx->timestamp_ms;
        s.first = 0;
        printf("[DifyPersonVerify] ch=%d init, ts=%llu\n", ctx->chnId, (unsigned long long)ctx->timestamp_ms);
        return;
    }

    int has_roi = (ctx->roi && ctx->roi->size() >= 3);

    /* 收集当前帧所有有效的人员 track_id */
    std::set<int> current_person_ids;
    int total_person = 0, no_track = 0, out_roi = 0;
    for (auto &r : *ctx->results)
    {
        if (r.label != "person")
            continue;
        total_person++;
        if (r.track_id < 0)
        {
            no_track++;
            continue;
        }
        if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
        {
            out_roi++;
            continue;
        }

        current_person_ids.insert(r.track_id);
        r.box_color = cv::Scalar(0, 0, 255);
    }

    /* ---- 更新 miss_frames ---- */
    /* 仍在画面中的: 清零 */
    for (int tid : current_person_ids)
        s.miss_frames[tid] = 0;

    /* 不在画面中但在 reported_ids / miss_frames 中的: 递增 */
    {
        std::vector<int> expired;
        for (auto &kv : s.miss_frames)
        {
            int tid = kv.first;
            if (current_person_ids.find(tid) != current_person_ids.end())
                continue; // 上面已清零
            kv.second++;
            if (kv.second > MISS_FRAME_MAX)
                expired.push_back(tid);
        }
        for (int tid : expired)
        {
            s.miss_frames.erase(tid);
            s.reported_ids.erase(tid);
        }
    }

    /* ---- 找出新出现的人员 (不在 reported_ids 中) ---- */
    bool has_new_person = false;
    int new_tid = -1;
    for (int tid : current_person_ids)
    {
        if (s.reported_ids.find(tid) == s.reported_ids.end())
        {
            has_new_person = true;
            new_tid = tid;
            break;
        }
    }

    /* ---- 上报至 Dify ---- */
    if (has_new_person && (ctx->timestamp_ms - s.last_upload_ms >= MIN_INTERVAL_MS))
    {
        const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                                 ? ctx->config->dify_prompt.c_str()
                                 : "person detected, please verify";

        char event_id[128];
        snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu_person",
                 ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);

        /* 克隆帧并在上面绘制所有目标框 + id/类别/置信度 */
        cv::Mat upload_img = ctx->frame->clone();
        int img_w = upload_img.cols;
        int img_h = upload_img.rows;

        for (auto &r : *ctx->results)
        {
            /* 将检测框 clamp 到图像边界内 */
            int bx = std::max(0, r.box.x);
            int by = std::max(0, r.box.y);
            int bw = std::min(r.box.width, img_w - bx);
            int bh = std::min(r.box.height, img_h - by);
            if (bw <= 0 || bh <= 0)
                continue;

            cv::Rect safe_box(bx, by, bw, bh);

            cv::Scalar color;
            int thickness;
            if (r.label == "person")
            {
                color = cv::Scalar(0, 0, 255);
                thickness = 2;
            }
            else
            {
                color = cv::Scalar(0, 255, 0);
                thickness = 1;
            }

            cv::rectangle(upload_img, safe_box, color, thickness);

            char label_text[96];
            if (r.track_id >= 0)
                snprintf(label_text, sizeof(label_text), "id:%d %s %.2f", r.track_id, r.label.c_str(), r.score);
            else
                snprintf(label_text, sizeof(label_text), "%s %.2f", r.label.c_str(), r.score);

            int baseline = 0;
            cv::Size text_sz = cv::getTextSize(label_text, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &baseline);
            int txt_w = text_sz.width + 4;
            int txt_h = text_sz.height + 4;

            /* 文字放在框上方，若上方空间不够则放在框内顶部 */
            int txt_x = std::max(0, bx);
            int txt_y = by - 6;
            if (txt_y - txt_h < 0)
                txt_y = by + txt_h + 2;

            /* clamp 文字 ROI 到图像边界 */
            txt_x = std::min(txt_x, img_w - txt_w);
            txt_y = std::min(std::max(txt_y, txt_h), img_h);

            cv::Rect roi_rect(txt_x, txt_y - txt_h, txt_w, txt_h);
            cv::Mat roi = upload_img(roi_rect);
            cv::Mat overlay;
            roi.copyTo(overlay);
            cv::rectangle(overlay, cv::Rect(0, 0, roi.cols, roi.rows), color, -1);
            cv::addWeighted(overlay, 0.5, roi, 0.5, 0, roi);

            draw_text_unicode(upload_img, label_text,
                              cv::Point(txt_x + 2, txt_y - 4),
                              /*px*/ 14, cv::Scalar(255, 255, 255), /*filled*/ -1);
        }

        printf("[DifyPersonVerify] ch=%d UPLOAD: new_tid=%d frame=%lld ts=%llu\n",
               ctx->chnId, new_tid, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);
        const char *dify_url = (ctx->config && !ctx->config->dify_api_url.empty()) ? ctx->config->dify_api_url.c_str() : nullptr;
        const char *dify_key = (ctx->config && !ctx->config->dify_api_key.empty()) ? ctx->config->dify_api_key.c_str() : nullptr;
        dify_uploader_enqueue(upload_img, prompt, event_id, dify_url, dify_key);
        s.last_upload_ms = ctx->timestamp_ms;

        /* 把当前所有可见 track_id 都标记为已上报, 防止同一帧多人轮流触发 */
        for (int tid : current_person_ids)
        {
            s.reported_ids.insert(tid);
            s.miss_frames[tid] = 0;
        }
    }

    /* ---- 定期调试打印 ---- */
    if (ctx->timestamp_ms - s.last_dbg_ms >= DBG_INTERVAL_MS)
    {
        printf("[DifyPersonVerify] ch=%d frame=%lld | total_person=%d no_track=%d out_roi=%d | "
               "tracked=%zu reported=%zu miss=%zu | last_up=%llu\n",
               ctx->chnId, (long long)ctx->frame_id,
               total_person, no_track, out_roi,
               current_person_ids.size(), s.reported_ids.size(), s.miss_frames.size(),
               (unsigned long long)s.last_upload_ms);
        s.last_dbg_ms = ctx->timestamp_ms;
    }

    /* ---- 状态绘制 ---- */
    char status[128];
    snprintf(status, sizeof(status), "DifyVerify: %zu person(s) | rep:%zu",
             current_person_ids.size(), s.reported_ids.size());
    draw_text(ctx, status, cv::Point(20, 30),
              current_person_ids.empty() ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255),
              0.6, 2);

    /* 为每个检测到的人员绘制 track_id */
    for (auto &r : *ctx->results)
    {
        if (r.label != "person" || r.track_id < 0)
            continue;
        if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
            continue;

        char tid_text[32];
        snprintf(tid_text, sizeof(tid_text), "id:%d", r.track_id);
        draw_text(ctx, tid_text,
                  cv::Point(r.box.x, std::max(20, r.box.y - 8)),
                  cv::Scalar(0, 255, 255), 0.45, 1);
    }
}

REGISTER_LOGIC("logic_dify_person_verify", logic_dify_person_verify);
