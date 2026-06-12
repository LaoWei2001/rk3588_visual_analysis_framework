/**
 * @file logic_wafer.cpp
 * logic_wafer —— 晶圆盘擦拭工序管理(轨迹/覆盖率/动作识别/计时/合规报警)
 *
 * 背景：AI 视觉管理"人工用工具擦拭晶圆盘"工序。模型类别：0=cleanwiper(工具尖端), 1=wafer(晶圆盘)。
 * 计算区域 = wafer(class 1) 检测框本身(动态)，不用手画 ROI。
 * 流程(状态机)：
 *   - 工序开始：cleanwiper 中心进入 wafer 框并持续 >= t_start 秒 → 开始(计时/记轨迹/算覆盖)。
 *     确认期滤除手短暂进入、反光/闪光误检；期间不画不计。
 *   - 进行中：每帧把工具中心连成轨迹，按 line_width 拓宽成"扫过面积带"(单色半透明铺底，
 *     不再实时分类/染色)；左上实时显示 覆盖率(以实时 wafer 框为基准)/用时/状态。
 *   - 工序结束：工具离开区域或消失持续 >= t_end 秒 → 结算：① 覆盖率(以"结束那一刻 wafer 框"
 *     为基准) >= coverage_threshold；② 把整条轨迹交给 wafer_detect_missing(点集, required) 离线
 *     分析有无漏动作(横/纵/弧)。两者都过 → 合格；否则右上角红字报警(驻留到下一工序)。
 * 不上报；坐标全部在模型输入坐标系(640)，与检测 box 一致。可调参数见 logics.json。
 */
#include "logic_common.h"

struct WaferState
{
    std::vector<cv::Point> trajectory;  /* cleanwiper 在区域内的轨迹点(模型坐标, 顺序即编号) */

    cv::Mat   swept_mask;               /* 扫过掩码(CV_8UC1)，每帧由轨迹重建 */
    cv::Size  mask_size;                /* swept_mask 对应帧尺寸 */
    cv::Rect  region_box;               /* 计算区域 = wafer(class 1)检测框(最近一次) */
    bool      have_region    = false;

    /* 开始(start)/结束(end) 计时 */
    uint64_t  enter_ms       = 0;       /* 工序开始确认窗口起点(0=未在确认) */
    uint64_t  region_last_ms = 0;       /* 最近一次在区域内的时刻(开始/结束判定共用) */
    bool      op_active      = false;   /* 是否已确认并在进行一次工序 */
    uint64_t  op_start_ms    = 0;       /* 工序开始时刻(确认通过) */

    /* 上次工序结算结果(驻留显示) */
    bool        have_result   = false;
    bool        last_pass     = true;
    float       last_coverage = 0.0f;   /* % */
    float       last_op_sec   = 0.0f;
    std::string last_msg;               /* 不合规原因(多行用 '\n' 分隔, ASCII) */
};

/* 动作英文短名(屏幕用 Hershey 字体不支持中文，故用 ASCII)；1横/2纵/3弧 */
static const char *wafer_action_name(int a)
{
    switch (a) { case 1: return "H"; case 2: return "V"; case 3: return "ARC"; default: return "?"; }
}

/* ===== 动作识别(仅结算时离线分析整条轨迹，不再实时分类/染色) ===== */

/* 一个窗口的运动学特征 */
struct WaferMotionFeat
{
    bool   valid       = false;
    double extent      = 0.0;   /* 主轴方向标准差(像素) */
    double linearity   = 0.0;   /* 1 - λ2/λ1 (1=共线/直线, →0=各向同性) */
    double trav_dx     = 0.0;   /* 窗口内 X 方向逐步行程 Σ|dx|(判横/纵, 抗整体漂移) */
    double trav_dy     = 0.0;   /* 窗口内 Y 方向逐步行程 Σ|dy| */
    double circ_resid  = 1e9;   /* 圆拟合归一化残差 rms|dist-R|/R(越小越像圆) */
    double circ_R      = 0.0;   /* 拟合半径(像素) */
    double arc_path    = 0.0;   /* 沿圆累计角程(弧度) */
    int    reversals   = 0;     /* 主轴投影方向反转次数(往复=擦) */
    double periodicity = 0.0;   /* 频域主频(非DC)能量占比 0~1(越大越周期/往复) */
};

/* 对一段连续点 p[0..m-1] 算特征：PCA(线性) + 逐步行程(横/纵) + Kåsa 圆拟合(残差/角程)
 * + 主轴投影反转计数 与 DFT 主频能量占比(频域周期性)。 */
static WaferMotionFeat wafer_motion_features(const cv::Point *p, int m)
{
    WaferMotionFeat f;
    if (!p || m < 6) return f;

    /* 均值 + 中心化二阶矩 */
    double mx = 0.0, my = 0.0;
    for (int i = 0; i < m; ++i) { mx += p[i].x; my += p[i].y; }
    mx /= m; my /= m;
    double Suu = 0, Svv = 0, Suv = 0, Suz = 0, Svz = 0, Sz = 0;
    for (int i = 0; i < m; ++i)
    {
        double u = p[i].x - mx, v = p[i].y - my, z = u * u + v * v;
        Suu += u * u; Svv += v * v; Suv += u * v;
        Suz += u * z; Svz += v * z; Sz += z;
    }

    /* PCA：特征值 + 主轴方向 */
    double tr = Suu + Svv, det2 = Suu * Svv - Suv * Suv;
    double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det2));
    double l1 = tr / 2.0 + disc, l2 = tr / 2.0 - disc;
    if (l1 < 1e-9) return f;
    f.extent    = std::sqrt(l1 / m);
    f.linearity = 1.0 - (l2 / l1);
    double th = 0.5 * std::atan2(2.0 * Suv, Suu - Svv);

    /* 逐步行程(判横/纵)：捕捉来回往复的方向，抗"擦时整体漂移"，比位置散布更准 */
    double trav_dx = 0.0, trav_dy = 0.0;
    for (int i = 1; i < m; ++i)
    {
        trav_dx += std::fabs(static_cast<double>(p[i].x - p[i - 1].x));
        trav_dy += std::fabs(static_cast<double>(p[i].y - p[i - 1].y));
    }
    f.trav_dx = trav_dx; f.trav_dy = trav_dy;

    /* Kåsa 圆拟合(中心化坐标)：解 2x2 得圆心(uc,vc)、半径 R、归一化残差、角程 */
    double cdet = Suu * Svv - Suv * Suv;
    if (std::fabs(cdet) > 1e-6)
    {
        double uc = (Svv * (0.5 * Suz) - Suv * (0.5 * Svz)) / cdet;
        double vc = (Suu * (0.5 * Svz) - Suv * (0.5 * Suz)) / cdet;
        double R2 = uc * uc + vc * vc + Sz / m;
        if (R2 > 1e-6)
        {
            double R = std::sqrt(R2); f.circ_R = R;
            double se = 0.0, prevphi = 0.0, arc = 0.0; bool hp = false;
            for (int i = 0; i < m; ++i)
            {
                double u = p[i].x - mx, v = p[i].y - my;
                double du = u - uc, dv = v - vc, dist = std::sqrt(du * du + dv * dv);
                se += (dist - R) * (dist - R);
                double phi = std::atan2(dv, du);
                if (hp) { double d = phi - prevphi; while (d > CV_PI) d -= 2 * CV_PI; while (d < -CV_PI) d += 2 * CV_PI; arc += std::fabs(d); }
                prevphi = phi; hp = true;
            }
            f.circ_resid = std::sqrt(se / m) / R;
            f.arc_path   = arc;
        }
    }

    /* 主轴投影 → 反转次数(带死区) + 频域主频能量占比(DFT) */
    double ux = std::cos(th), uy = std::sin(th);
    std::vector<float> proj(m);
    for (int i = 0; i < m; ++i) proj[i] = static_cast<float>((p[i].x - mx) * ux + (p[i].y - my) * uy);

    double dead = 0.15 * f.extent + 1.0;
    int rev = 0; double lastdir = 0.0;
    for (int i = 1; i < m; ++i)
    {
        double dd = proj[i] - proj[i - 1];
        if (std::fabs(dd) < dead) continue;
        double dir = (dd > 0) ? 1.0 : -1.0;
        if (lastdir != 0.0 && dir != lastdir) ++rev;
        lastdir = dir;
    }
    f.reversals = rev;

    {
        double pm = 0.0; for (float x : proj) pm += x; pm /= m;
        std::vector<float> sig(m);
        for (int i = 0; i < m; ++i)
        {
            double w = 0.5 - 0.5 * std::cos(2.0 * CV_PI * i / (m - 1));   /* Hann 窗 */
            sig[i] = static_cast<float>((proj[i] - pm) * w);
        }
        cv::Mat spec;
        cv::dft(cv::Mat(sig), spec, cv::DFT_COMPLEX_OUTPUT);
        double tot = 0.0, best = 0.0;
        for (int k = 1; k <= m / 2; ++k)
        {
            cv::Vec2f c = spec.at<cv::Vec2f>(k);
            double e = static_cast<double>(c[0]) * c[0] + static_cast<double>(c[1]) * c[1];
            tot += e; if (e > best) best = e;
        }
        f.periodicity = (tot > 1e-9) ? best / tot : 0.0;
    }

    f.valid = true;
    return f;
}

/* 一个窗口判一个标签：横擦(1)/纵擦(2)/圆弧擦(3)/未知(0)。 */
static int wafer_window_label(const cv::Point *p, int m)
{
    WaferMotionFeat f = wafer_motion_features(p, m);
    if (!f.valid || f.extent < 3.0) return 0;
    /* 圆弧：圆拟合残差小 + 角程足够 + 非共线 + 半径合理 */
    if (f.circ_resid < 0.12 && f.arc_path >= 1.0 && f.linearity < 0.93 && f.circ_R > 4.0) return 3;
    /* 直线往复门控(确认是"擦"而非噪声漂移) */
    bool wipe = (f.linearity > 0.6) || (f.reversals >= 1) || (f.periodicity > 0.35);
    if (!wipe) return 0;
    /* 横/纵：逐步行程 Σ|dx| vs Σ|dy|(抗整体漂移) */
    if (f.trav_dx >= f.trav_dy * 1.15) return 1;
    if (f.trav_dy >= f.trav_dx * 1.15) return 2;
    return 0;
}

/* 轨迹点(坐标 + 编号)——结算分析函数的输入元素 */
struct WaferTrajPoint { cv::Point pt; int id; };

/* 解析 required_actions(逗号/空格/斜杠/分号分隔；支持 横擦/纵擦/圆弧擦 或 h/v/arc)
 * → 需校验的动作 id 列表(1=横,2=纵,3=弧)；空串 → 空列表(不校验)。 */
static std::vector<int> wafer_parse_required(const std::string &req)
{
    bool need_h = false, need_v = false, need_arc = false;
    std::string tok;
    auto flush = [&](std::string t)
    {
        size_t a = t.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return;
        size_t b = t.find_last_not_of(" \t\r\n");
        t = t.substr(a, b - a + 1);
        if (t.find("\xE6\xA8\xAA") != std::string::npos || t == "h" || t == "H") need_h = true;          /* 横 */
        else if (t.find("\xE7\xBA\xB5") != std::string::npos || t == "v" || t == "V") need_v = true;     /* 纵 */
        else if (t.find("\xE5\xBC\xA7") != std::string::npos || t.find("\xE5\x9C\x86") != std::string::npos
                 || t == "arc" || t == "ARC" || t == "a" || t == "A") need_arc = true;                   /* 弧/圆 */
    };
    for (char ch : req)
    {
        if (ch == ',' || ch == ' ' || ch == '/' || ch == ';' || ch == '\t' || ch == '\n')
        { flush(tok); tok.clear(); }
        else tok.push_back(ch);
    }
    flush(tok);

    std::vector<int> out;
    if (need_h)   out.push_back(1);
    if (need_v)   out.push_back(2);
    if (need_arc) out.push_back(3);
    return out;
}

/* 【结算时离线判漏动作】——按需求设计的核心函数。
 *   输入1 pts      : 有序轨迹点集合(每个含坐标 pt + 编号 id)；
 *   输入2 required : 需校验的动作 id 列表(0/1/多个；空=不校验)；
 *   返回           : required 中"没出现过"的动作 id 列表(缺失项)。
 * 方法：对整条轨迹滑动窗口逐段分类(圆拟合判弧 / 逐步行程判横纵)，统计各动作出现的
 *      窗口数，达到支持度阈值即认定该动作出现过，最后求缺失。 */
static std::vector<int> wafer_detect_missing(const std::vector<WaferTrajPoint> &pts,
                                             const std::vector<int> &required)
{
    if (required.empty()) return {};

    std::vector<cv::Point> xy; xy.reserve(pts.size());     /* 按编号取有序坐标(轨迹本就有序) */
    for (const auto &q : pts) xy.push_back(q.pt);
    const int n = static_cast<int>(xy.size());

    const int WIN = 28, STEP = 4;
    int cnt[4] = {0, 0, 0, 0};
    int nwin = 0;
    for (int e = WIN; e <= n; e += STEP)
    {
        int lab = wafer_window_label(xy.data() + (e - WIN), WIN);
        if (lab >= 1 && lab <= 3) ++cnt[lab];
        ++nwin;
    }
    if (nwin == 0 && n >= 6)            /* 轨迹太短不足一窗：整体判一次 */
    {
        int lab = wafer_window_label(xy.data(), n);
        if (lab >= 1 && lab <= 3) ++cnt[lab];
        nwin = 1;
    }

    /* 支持度：>=2 个窗口、或 >=12% 的窗口判为该动作，才算"出现过" */
    const int need = std::max(2, static_cast<int>(std::lround(0.12 * nwin)));
    std::vector<int> miss;
    for (int a : required)
        if (a >= 1 && a <= 3 && cnt[a] < need) miss.push_back(a);
    return miss;
}

static void logic_wafer(ChannelContext *ctx)
{
    if (!ctx || !ctx->results) return;
    if (!ctx->frame || ctx->frame->empty())
    {
        draw_text(ctx, "logic_wafer: frame empty", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    if (!*ctx->state) *ctx->state = std::make_shared<WaferState>();
    auto &s = *std::static_pointer_cast<WaferState>(*ctx->state);

    const cv::Size frame_size = ctx->frame->size();
    if (s.swept_mask.empty() || s.mask_size != frame_size)
    {
        s.swept_mask = cv::Mat::zeros(frame_size, CV_8UC1);
        s.mask_size  = frame_size;
    }

    /* ---- 可调参数(每帧现读，热重载) ---- */
    int line_width = ctx->config ? ctx->config->line_width : 20;
    if (line_width < 1) line_width = 1;
    float t_start = ctx->config ? ctx->config->t_start : 1.0f;
    if (t_start < 0.0f) t_start = 0.0f;
    float t_end = ctx->config ? ctx->config->t_end : 1.0f;
    if (t_end < 0.1f) t_end = 0.1f;
    float cov_thresh = ctx->config ? ctx->config->coverage_threshold : 80.0f;
    const std::string req_actions = ctx->config ? ctx->config->required_actions : std::string();
    const uint64_t t_start_ms = static_cast<uint64_t>(t_start * 1000.0f);
    const uint64_t t_end_ms   = static_cast<uint64_t>(t_end   * 1000.0f);
    const uint64_t now = ctx->timestamp_ms;

    /* ---- 计算区域 = wafer(class 1) 最高分框(缓存最近一次) ---- */
    float best_region_score = -1.0f;
    for (auto &r : *ctx->results)
    {
        if (r.class_id != 1) continue;
        if (r.score > best_region_score) { best_region_score = r.score; s.region_box = r.box; s.have_region = true; }
    }
    cv::Rect region;
    if (s.have_region)
        region = s.region_box & cv::Rect(0, 0, frame_size.width, frame_size.height);
    if (region.area() <= 0)
    {
        draw_text(ctx, "logic_wafer: waiting for wafer (region)", cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.6, 2);
        return;
    }

    /* ---- cleanwiper(class 0)：是否在画面 / 区域内最高分点 ---- */
    bool found_in_frame = false, have_new_pt = false;
    cv::Point new_pt; float best_score = -1.0f;
    for (auto &r : *ctx->results)
    {
        if (r.class_id != 0) continue;          /* 0=cleanwiper(按下标，比类名稳) */
        found_in_frame = true;
        r.box_color = cv::Scalar(0, 255, 0);
        const cv::Point c = r.box_center();
        if (region.contains(c) && r.score > best_score) { best_score = r.score; new_pt = c; have_new_pt = true; }
    }
    if (have_new_pt) s.region_last_ms = now;

    /* ---- 工序开始(T_start)：cleanwiper 在区域内持续 t_start 才正式开始 ---- */
    const uint64_t region_grace_ms = 400;       /* 确认期内区域短暂丢检的容忍 */
    if (!s.op_active)
    {
        if (have_new_pt) { if (s.enter_ms == 0) s.enter_ms = now; }
        else if (now - s.region_last_ms > region_grace_ms) s.enter_ms = 0;

        if (have_new_pt && s.enter_ms != 0 && (now - s.enter_ms >= t_start_ms))
        {
            s.op_active   = true;
            s.op_start_ms = now;
            s.trajectory.clear();
            s.have_result = false;              /* 新工序 → 撤掉上次告警 */
        }
    }

    /* ---- 记录新点(仅工序进行中；不实时分类，结算时统一离线分析) ---- */
    if (s.op_active && have_new_pt)
    {
        bool add = s.trajectory.empty();
        if (!add)
        {
            const cv::Point d = new_pt - s.trajectory.back();
            if (d.x * d.x + d.y * d.y >= 9) add = true;     /* 位移 >=3px 才记 */
        }
        if (add) s.trajectory.push_back(new_pt);
    }

    /* ---- 扫过掩码 + 实时覆盖率(以实时 wafer 框为基准) ---- */
    double ratio = 0.0;
    s.swept_mask.setTo(0);
    if (s.trajectory.size() == 1)
        cv::circle(s.swept_mask, s.trajectory[0], line_width / 2, cv::Scalar(255), -1);
    else if (s.trajectory.size() >= 2)
        cv::polylines(s.swept_mask, s.trajectory, false, cv::Scalar(255), line_width, cv::LINE_8);
    if (!s.trajectory.empty())
    {
        const double region_area = region.area();
        if (region_area > 0.0) ratio = cv::countNonZero(s.swept_mask(region)) / region_area;
    }

    /* ---- 工序结束(T_end)：cleanwiper 离开区域/消失持续 t_end → 结算 ---- */
    float end_elapsed = 0.0f;
    if (s.op_active)
    {
        end_elapsed = (now - s.region_last_ms) / 1000.0f;
        if (now - s.region_last_ms >= t_end_ms)
        {
            /* 覆盖率以"结束此刻 wafer 框"为基准(即当前 region，已在 ratio 中) */
            s.last_coverage = static_cast<float>(ratio * 100.0);
            s.last_op_sec   = (s.region_last_ms - s.op_start_ms) / 1000.0f;

            bool pass = true; std::string msg;
            if (s.last_coverage + 1e-3f < cov_thresh)
            {
                pass = false;
                char b[64]; snprintf(b, sizeof(b), "Coverage %.0f%% < %.0f%%", s.last_coverage, cov_thresh);
                msg = b;
            }
            /* 离线判漏动作：构造(坐标+编号)点集 → 调判定函数 wafer_detect_missing */
            std::vector<int> required = wafer_parse_required(req_actions);
            if (!required.empty())
            {
                std::vector<WaferTrajPoint> pts; pts.reserve(s.trajectory.size());
                for (int i = 0; i < static_cast<int>(s.trajectory.size()); ++i)
                    pts.push_back({s.trajectory[i], i});
                std::vector<int> miss = wafer_detect_missing(pts, required);
                if (!miss.empty())
                {
                    pass = false;
                    std::string ms;
                    for (size_t i = 0; i < miss.size(); ++i) { if (i) ms += ","; ms += wafer_action_name(miss[i]); }
                    if (!msg.empty()) msg += "\n";
                    msg += "Missing: " + ms;
                }
            }
            s.last_pass   = pass;
            s.last_msg    = msg;
            s.have_result = true;

            s.op_active = false;          /* 复位，等待下一工序 */
            s.trajectory.clear();
            s.enter_ms = 0;
        }
    }

    /* ---- 绘制：单色半透明轨迹(底层) → 区域框 → 文字 ---- */
    const cv::Scalar trail_color(255, 0, 255);   /* 品红，单色(不再按动作染色) */
    if (s.trajectory.size() == 1)
        draw_circle(ctx, s.trajectory[0], line_width / 2, trail_color, -1);
    else if (s.trajectory.size() >= 2)
        draw_polyline(ctx, s.trajectory, trail_color, line_width, 0.5);

    draw_rect(ctx, s.region_box, cv::Scalar(0, 255, 255), 2);   /* wafer 框=测量区域(黄) */

    /* 左上角：覆盖率 / 状态+动作 / 用时 */
    const float disp_cov = s.op_active ? static_cast<float>(ratio * 100.0)
                                       : (s.have_result ? s.last_coverage : 0.0f);
    char l1[80];
    snprintf(l1, sizeof(l1), "Coverage: %.1f%% / %.0f%%", disp_cov, cov_thresh);
    draw_text(ctx, l1, cv::Point(20, 30),
              (disp_cov + 1e-3f >= cov_thresh) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 200, 255), 0.7, 2);

    char l2[120]; cv::Scalar l2c;
    if (s.op_active && have_new_pt)
    { snprintf(l2, sizeof(l2), "TRACKING pts:%zu", s.trajectory.size()); l2c = cv::Scalar(0, 255, 0); }
    else if (s.op_active)
    { snprintf(l2, sizeof(l2), "ENDING %.1f/%.1fs", end_elapsed, t_end); l2c = cv::Scalar(0, 200, 255); }
    else if (s.enter_ms != 0)
    { float dw = (now - s.enter_ms) / 1000.0f; snprintf(l2, sizeof(l2), "CONFIRMING %.1f/%.1fs", dw, t_start); l2c = cv::Scalar(0, 165, 255); }
    else if (found_in_frame)
    { snprintf(l2, sizeof(l2), "wiper out of region"); l2c = cv::Scalar(180, 180, 180); }
    else
    { snprintf(l2, sizeof(l2), "idle"); l2c = cv::Scalar(180, 180, 180); }
    draw_text(ctx, l2, cv::Point(20, 60), l2c, 0.6, 2);

    char l3[64];
    if (s.op_active)        { float run = (now - s.op_start_ms) / 1000.0f; snprintf(l3, sizeof(l3), "Time: %.1fs", run); }
    else if (s.have_result)  snprintf(l3, sizeof(l3), "Last: %.1fs", s.last_op_sec);
    else                     snprintf(l3, sizeof(l3), "Time: 0.0s");
    draw_text(ctx, l3, cv::Point(20, 90), cv::Scalar(0, 255, 255), 0.7, 2);

    /* 右上角：结算结果(不合规红字驻留, 合格绿字; 直到下一工序开始) */
    if (s.have_result && !s.op_active)
    {
        std::vector<std::string> lines;
        if (s.last_pass) lines.push_back("PASS");
        else
        {
            lines.push_back("NG");
            size_t p0 = 0;
            while (p0 <= s.last_msg.size())
            {
                size_t nl = s.last_msg.find('\n', p0);
                std::string ln = s.last_msg.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0);
                if (!ln.empty()) lines.push_back(ln);
                if (nl == std::string::npos) break;
                p0 = nl + 1;
            }
        }
        const cv::Scalar c = s.last_pass ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        for (size_t k = 0; k < lines.size(); ++k)
        {
            int base = 0; const double fs = 0.7; const int th = 2;
            cv::Size ts = cv::getTextSize(lines[k], cv::FONT_HERSHEY_SIMPLEX, fs, th, &base);
            int x = std::max(10, frame_size.width - ts.width - 12);
            int y = 30 + static_cast<int>(k) * 30;
            draw_text(ctx, lines[k].c_str(), cv::Point(x, y), c, fs, th);
        }
    }
}

REGISTER_LOGIC("logic_wafer", logic_wafer);
