/**
 * @file logic_lesson10.cpp
 * @brief logic_lesson10 —— 利用帧差法展示如何利用传统的cv视觉算法分析视频帧。
 */
#include "logic_common.h"

/* 上一帧灰度图（跨帧保留） */
struct AbandonState
{
    cv::Ptr<cv::BackgroundSubtractorMOG2> mog2;
    cv::Mat prev_gray; // 上一帧灰度(算"当前在不在动")
    cv::Mat dwell;     // 每像素"已静止前景"累计秒数 (CV_32F)
    int frames = 0;
};

static void logic_lesson10(ChannelContext *ctx)
{
    if (!ctx || !ctx->frame || ctx->frame->empty())
        return;

    // ───── 参数 ─────
    const int linger_sec = (ctx->config && ctx->config->linger_sec > 0) ? ctx->config->linger_sec
                                                                        : 10; // ★遗留时间(秒)：网页/config 可改、热重载
    const bool BG_FREEZE = true; // ★学完背景后冻结：遗留物会一直保持前景，不会被"学进背景"而消失(关键修复)
    const double LEARN_INIT = 0.05; // 热身期学"空场景"的速率(快点把背景学好)
    const double VAR_THRESH = 16.0; // MOG2 前景灵敏度
    const int MOTION_THRESH = 25;   // 帧差阈值：判"当前是否在动"
    const double DECAY = 3.0; // 候选消失时 dwell 的衰减倍率(>1：消失比累计快，容忍短暂噪点闪烁)
    const double MIN_AREA = 500.0; // 遗留物最小面积，滤掉小噪点
    const int WARMUP = 40;         // 前 N 帧学背景(此间场景须保持空)，之后才开始判定

    // ───── 状态 ─────
    if (!*ctx->state)
        *ctx->state = std::make_shared<AbandonState>();
    auto &s = *std::static_pointer_cast<AbandonState>(*ctx->state);
    if (!s.mog2)
        s.mog2 = cv::createBackgroundSubtractorMOG2(500, VAR_THRESH, /*shadow=*/true);

    cv::Mat &vis = ctx->display_canvas(); // 显示画布(BGR, 当前帧副本)

    // 灰度(给帧差用)
    cv::Mat gray;
    cv::cvtColor(*ctx->frame, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0);

    // 1) 前景(MOG2) → 去阴影 → 开运算去噪
    //    学背景速率：热身期快速学空场景；之后冻结(0) →
    //    静止的遗留物会一直被当成前景，
    //    不会像之前那样几秒就被"学进背景"导致黄色消失、永远到不了红。
    double lr = (s.frames < WARMUP) ? LEARN_INIT : (BG_FREEZE ? 0.0 : 0.0008);
    cv::Mat fg;
    s.mog2->apply(*ctx->frame, fg, lr);
    cv::threshold(fg, fg, 200, 255, cv::THRESH_BINARY);
    cv::morphologyEx(fg, fg, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    ++s.frames;

    // 累加器 / 上一帧 初始化
    if (s.dwell.empty() || s.dwell.size() != fg.size())
        s.dwell = cv::Mat::zeros(fg.size(), CV_32FC1);
    if (s.prev_gray.empty() || s.prev_gray.size() != gray.size())
        s.prev_gray = gray.clone();

    // 2) 当前运动(帧差)；膨胀让运动区域胖一点，避免把"刚停下"误判成静止
    cv::Mat diff, motion;
    cv::absdiff(gray, s.prev_gray, diff);
    cv::threshold(diff, motion, MOTION_THRESH, 255, cv::THRESH_BINARY);
    cv::dilate(motion, motion, cv::Mat(), cv::Point(-1, -1), 2);

    // 3) 静止前景 = 前景 且 当前不动
    cv::Mat not_motion, static_fg;
    cv::bitwise_not(motion, not_motion);
    cv::bitwise_and(fg, not_motion, static_fg);

    // 4) 逐像素计时：静止前景处 dwell += dt；其它处衰减、夹回 0
    double dt = ctx->dt_ms / 1000.0;
    if (dt <= 0.0 || dt > 0.5)
        dt = 0.04;
    cv::Mat not_static;
    cv::bitwise_not(static_fg, not_static);
    cv::add(s.dwell, cv::Scalar(dt), s.dwell, static_fg); // 候选: +dt
    cv::subtract(s.dwell, cv::Scalar(dt * DECAY), s.dwell,
                 not_static);                                     // 非候选: 衰减
    cv::threshold(s.dwell, s.dwell, 0.0, 0.0, cv::THRESH_TOZERO); // 负数夹回 0

    // 5) dwell ≥ linger_sec → 遗留物掩码
    cv::Mat abandoned;
    cv::compare(s.dwell, (double)linger_sec, abandoned, cv::CMP_GE);
    cv::morphologyEx(abandoned, abandoned, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));

    // 显示：静止候选(计时中)淡黄、已判遗留物红
    cv::Mat tint = vis.clone();
    tint.setTo(cv::Scalar(0, 200, 200), static_fg); // 计时中：黄
    tint.setTo(cv::Scalar(0, 0, 255), abandoned);   // 已遗留：红
    cv::addWeighted(tint, 0.4, vis, 0.6, 0.0, vis);

    // 6) 热身期：背景没学好先不判定
    if (s.frames < WARMUP)
    {
        char w[64];
        snprintf(w, sizeof(w), "bg-model: learning %d/%d", s.frames, WARMUP);
        cv::putText(vis, w, cv::Point(20, 32), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
        s.prev_gray = gray.clone();
        return;
    }

    // 7) 遗留物轮廓 → 红框 + 计数
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(abandoned, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    int n = 0;
    for (const auto &c : contours)
    {
        if (cv::contourArea(c) < MIN_AREA)
            continue;
        cv::Rect box = cv::boundingRect(c);
        // 想排除"站着不动的人"：若该通道也跑了 YOLO，可在此判断 box 是否与某个
        // person 框重叠并 continue。
        cv::rectangle(vis, box, cv::Scalar(0, 0, 255), 3);
        cv::putText(vis, "LEFT OBJECT", cv::Point(box.x, std::max(20, box.y - 6)), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(0, 0, 255), 2);
        ++n;
    }

    char txt[80];
    snprintf(txt, sizeof(txt), "abandoned(>%ds): %d", linger_sec, n);
    cv::putText(vis, txt, cv::Point(20, 32), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                n > 0 ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0), 2);

    // 有遗留物就上报(可选；连了「上报配置」节点才真正发——把 report_enabled(ctx)
    // 作为参数传入)：
    //   if (n > 0) alarm_uploader_enqueue(vis, *ctx->frame, ctx->chnId,
    //   "abandoned_object", report_enabled(ctx));

    // 8) 更新"上一帧"
    s.prev_gray = gray.clone();
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行,
// 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson10", logic_lesson10);
