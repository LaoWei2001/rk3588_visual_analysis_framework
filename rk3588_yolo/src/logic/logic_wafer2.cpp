/* ============================================================================
 * @file logic_wafer2.cpp
 * logic_wafer2 —— 晶圆擦拭轨迹分段可视化 (src/logic/visualize_trajectories.py 移植)
 *
 * 模型类别: 0=cleanwiper(刮片), 1=wafer(晶圆盘) —— 与 logic_wafer 一样。
 *
 * 每帧流程(把 Python 离线脚本改为实时增量):
 *   ① 最高分 wafer 框 → 5 步法提取晶圆椭圆(裁剪→灰度→高斯模糊→暗区二值→开闭运算→
 *      碎片轮廓→凸包→fitEllipse), 面积突变剔除 + EMA 时序平滑(同脚本 extract_wafer_ellipse);
 *   ② 最高分刮片框中心追加到轨迹(t = 距本轮起点秒数);
 *   ③ 覆盖率: 500×500 归一化掩码累计 圆点+连线(刮片宽度固定16px), 单位圆内占比
 *      (同 compute_coverage_from_ellipse; 脚本用全视频中值椭圆做基准, 实时版用当前平滑椭圆);
 *   ④ 轨迹分段(segment_trajectory 逐行移植): 时间间隙预切(>=0.15s) → 折返率判
 *      横擦(linear)/环擦(spiral) → 长直线段按重平滑 PCA 角度突变再切 → 相邻短段合并 →
 *      边界 spiral 复判; 每新增 5 个点重算一次(脚本为全轨迹离线一次性计算);
 *   ⑤ 叠加: 红色虚线椭圆+「覆盖基准」 → 绿色半透明覆盖带 → 当前点(段色圆点+白圈) →
 *      最近 3s 分段轨迹 → 顶部图例(覆盖面积/刮片宽度/横擦·竖擦·环擦 模板槽位 ▶✓○)。
 *
 * 与脚本的差异(实时化所需):
 *   - 脚本跑完整段视频后回放绘制; 本逻辑边采集边算, 分段结果随轨迹增长而细化;
 *   - 覆盖带视觉效果用全程轨迹半透明粗折线近似(脚本是掩码反变形回椭圆内, 视觉接近);
 *   - 刮片消失超 5s 视为一轮结束并自动复位(脚本单视频离线, 无需复位; 循环流必须有);
 *   - YOLO 检测框由框架固定绘制(脚本不画), 此处仅改色: 刮片绿/晶圆灰。
 *
 * 参数暂写死(来源: 脚本命令行默认值 + config.yaml 中脚本实际用到的项):
 *   dark_threshold=105(--dark-thresh 默认; 注: config.yaml roi.dark_threshold=80 脚本并未使用),
 *   smooth_alpha=0.05(roi.smooth_alpha), trail_sec=3.0(--trail-sec), 刮片宽=16px(覆盖计算固定值),
 *   mask_res=500。config.yaml 的 tracking/sop 两节属其它脚本, visualize_trajectories.py 不读取;
 *   detector 节(conf=0.25/iou=0.45/imgsz=640)对应通道模型配置 → 该通道 obj_thresh 请设 0.25。
 * ==========================================================================*/
#include "logic_common.h"

struct W2Point { float x, y; double t; };          /* t: 秒, 相对本轮起点 */

struct W2Ellipse { double cx = 0, cy = 0, a = 0, b = 0, angle = 0; }; /* (a,b)=全轴长, 同脚本 */

enum W2Phase { W2_LINEAR = 0, W2_SPIRAL = 1, W2_SHORT = 2 };
struct W2Seg { int a; int b; int phase; };         /* [a,b] 为轨迹点下标(闭区间) */

/* ---- numpy 等价小工具 ---- */
static double w2_median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return (v.size() % 2) ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

static double w2_percentile(std::vector<double> v, double q)   /* np.percentile 线性插值 */
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = q / 100.0 * (double)(v.size() - 1);
    const size_t lo = (size_t)idx;
    if (lo + 1 >= v.size()) return v.back();
    return v[lo] + (idx - (double)lo) * (v[lo + 1] - v[lo]);
}

/* 滑动窗折返率: 步进方向变化 >150° 记一次折返, 窗口 [t-w/2, t+w/2] 内 折返数/时长。
 * 窗内点数 <5 或时长 <=0.3s → NAN(无效)。对应脚本 compute_reversal_rate(双指针等价加速)。 */
static std::vector<double> w2_reversal_rate(const W2Point *p, int n, double window_sec)
{
    std::vector<double> rate((size_t)std::max(n, 0), NAN);
    if (n <= 1) return rate;
    std::vector<unsigned char> rev((size_t)n, 0);
    double last_angle = NAN;
    for (int i = 1; i < n; ++i)
    {
        const double dx = p[i].x - p[i - 1].x, dy = p[i].y - p[i - 1].y;
        if (std::fabs(dx) < 1.0 && std::fabs(dy) < 1.0) continue;
        const double ang = std::atan2(dy, dx) * 180.0 / CV_PI;
        if (!std::isnan(last_angle))
        {
            double diff = std::fabs(ang - last_angle);
            if (diff > 180.0) diff = 360.0 - diff;
            if (diff > 150.0) rev[i] = 1;
        }
        last_angle = ang;
    }
    int lo = 0, hi = 0, revs = 0;                  /* [lo, hi) 滑动窗, t 单调递增 */
    for (int i = 0; i < n; ++i)
    {
        const double wl = p[i].t - window_sec * 0.5, wh = p[i].t + window_sec * 0.5;
        while (hi < n && p[hi].t <= wh) { revs += rev[hi]; ++hi; }
        while (lo < hi && p[lo].t < wl) { revs -= rev[lo]; ++lo; }
        if (hi - lo >= 5)
        {
            const double dur = p[hi - 1].t - p[lo].t;
            if (dur > 0.3) rate[i] = (double)revs / dur;
        }
    }
    return rate;
}

/* 每点 ±half_window 小窗 PCA 主轴角(mod 180°), 再做 kernel 点零填充滑动平均
 * (同 np.convolve mode='same')。输出仅含有效点(窗内>=5点)的 (t, 平滑角)。
 * 对应脚本 local_pca_angle_smoothed。 */
static void w2_pca_angle_smoothed(const W2Point *p, int n, int half_window, int kernel,
                                  std::vector<double> &out_ts, std::vector<double> &out_ang)
{
    out_ts.clear(); out_ang.clear();
    std::vector<double> ang;
    for (int i = 0; i < n; ++i)
    {
        const int lo = std::max(0, i - half_window), hi = std::min(n, i + half_window + 1);
        const int m = hi - lo;
        if (m < 5) continue;
        double cx = 0, cy = 0;
        for (int j = lo; j < hi; ++j) { cx += p[j].x; cy += p[j].y; }
        cx /= m; cy /= m;
        double sxx = 0, syy = 0, sxy = 0;
        for (int j = lo; j < hi; ++j)
        {
            const double u = p[j].x - cx, v = p[j].y - cy;
            sxx += u * u; syy += v * v; sxy += u * v;
        }
        double a = 0.5 * std::atan2(2.0 * sxy, sxx - syy) * 180.0 / CV_PI;
        while (a >= 90.0) a -= 180.0;
        while (a < -90.0) a += 180.0;
        out_ts.push_back(p[i].t);
        ang.push_back(a);
    }
    const int vn = (int)ang.size(), half = kernel / 2;
    out_ang.assign((size_t)vn, 0.0);
    for (int i = 0; i < vn; ++i)
    {
        double sum = 0;
        for (int k = -half; k <= half; ++k)
        {
            const int j = i + k;
            if (j >= 0 && j < vn) sum += ang[j];
        }
        out_ang[i] = sum / (double)kernel;         /* 边缘零填充: 恒除 kernel, 同 np.convolve */
    }
}

/* 轨迹分段 —— 脚本 segment_trajectory 逐行移植。
 * 输入按时间升序的检出点; 返回按起点排序的 (a,b,phase) 段列表。 */
static std::vector<W2Seg> w2_segment_trajectory(const std::vector<W2Point> &pts)
{
    const int n = (int)pts.size();
    std::vector<W2Seg> out;
    if (n <= 0) return out;
    if (n < 15) { out.push_back({0, n - 1, W2_LINEAR}); return out; }

    /* ---- ① 按细微时间间隙(>=0.15s)预切 ---- */
    std::vector<std::pair<int, int>> time_segs;
    int seg_start = 0;
    for (int i = 1; i < n; ++i)
    {
        if (pts[i].t - pts[i - 1].t >= 0.15)
        {
            if (i - 1 - seg_start >= 3) time_segs.push_back({seg_start, i - 1});
            seg_start = i;
        }
    }
    if (n - 1 - seg_start >= 3) time_segs.push_back({seg_start, n - 1});
    if (time_segs.empty()) time_segs.push_back({0, n - 1});

    std::vector<W2Seg> all;

    for (const auto &tspan : time_segs)
    {
        const int ts_a = tspan.first, ts_b = tspan.second;
        const W2Point *sub = pts.data() + ts_a;
        const int sub_n = ts_b - ts_a + 1;

        if (sub_n < 10) { all.push_back({ts_a, ts_b, W2_SHORT}); continue; }

        /* ---- ② 折返率判 linear/spiral ---- */
        const std::vector<double> rate = w2_reversal_rate(sub, sub_n, 1.5);
        std::vector<double> valid_rates;
        int first_valid = -1, last_valid = -1;
        for (int i = 0; i < sub_n; ++i)
            if (!std::isnan(rate[i]))
            {
                valid_rates.push_back(rate[i]);
                if (first_valid < 0) first_valid = i;
                last_valid = i;
            }
        if ((int)valid_rates.size() < 10) { all.push_back({ts_a, ts_b, W2_LINEAR}); continue; }

        /* 自适应阈值: 90 分位的 30%, 下限 0.5 */
        const double thr = std::max(0.5, 0.30 * w2_percentile(valid_rates, 90.0));

        /* 0.8s 分块取中值折返率 */
        struct W2Block { double rate; int start, end, phase; };
        std::vector<W2Block> blocks;
        const double block_sec = 0.8;
        for (double t0 = sub[first_valid].t; t0 < sub[last_valid].t; t0 += block_sec)
        {
            std::vector<double> rs;
            int bs = -1, be = -1;
            for (int j = 0; j < sub_n; ++j)
            {
                if (std::isnan(rate[j])) continue;
                if (sub[j].t >= t0 && sub[j].t < t0 + block_sec)
                {
                    rs.push_back(rate[j]);
                    if (bs < 0) bs = j;
                    be = j;
                }
            }
            if ((int)rs.size() >= 3) blocks.push_back({w2_median(rs), bs, be, 0});
        }

        if ((int)blocks.size() < 2)
        {
            const int ph = (!blocks.empty() && blocks[0].rate > thr) ? W2_LINEAR : W2_SPIRAL;
            all.push_back({ts_a, ts_b, ph});
            continue;
        }
        for (auto &b : blocks) b.phase = (b.rate >= thr) ? W2_LINEAR : W2_SPIRAL;

        /* 相位变化处分界 → 块组 → rr_segs */
        std::vector<int> bounds;
        bounds.push_back(0);
        for (int i = 1; i < (int)blocks.size(); ++i)
            if (blocks[i].phase != blocks[i - 1].phase) bounds.push_back(i);
        bounds.push_back((int)blocks.size());

        std::vector<W2Seg> rr_segs;
        for (int j = 0; j + 1 < (int)bounds.size(); ++j)
        {
            const int bs = bounds[j], be = bounds[j + 1] - 1;
            if (be < bs) continue;
            const int seg_s = blocks[bs].start, seg_e = blocks[be].end;
            if (sub[seg_e].t - sub[seg_s].t >= 0.3)
                rr_segs.push_back({seg_s, seg_e, blocks[bs].phase});
        }

        /* ---- ③ 长 linear 段(>3s)按重平滑 PCA 角度突变再切 ---- */
        std::vector<double> v_ts, v_ang;
        w2_pca_angle_smoothed(sub, sub_n, 12, 15, v_ts, v_ang);

        for (const auto &rs : rr_segs)
        {
            const int a = rs.a, b = rs.b;
            const double seg_dur = sub[b].t - sub[a].t;
            if (rs.phase != W2_LINEAR || seg_dur < 3.0)
            {
                all.push_back({ts_a + a, ts_a + b, rs.phase});
                continue;
            }

            std::vector<double> sa, st;
            for (size_t k = 0; k < v_ts.size(); ++k)
                if (v_ts[k] >= sub[a].t && v_ts[k] <= sub[b].t) { sa.push_back(v_ang[k]); st.push_back(v_ts[k]); }
            const int n_ang = (int)sa.size();
            if (n_ang < 15)
            {
                all.push_back({ts_a + a, ts_a + b, rs.phase});
                continue;
            }

            /* 找最大持续角度漂移(跳过段首 25% 和段尾 15%) */
            const double search_lo = sub[a].t + std::max(1.8, 0.25 * seg_dur);
            const double search_hi = sub[b].t - std::max(0.5, 0.15 * seg_dur);
            const int half_w = std::max(3, n_ang / 6);
            double best_shift = 0.0, best_split_t = NAN;
            for (int i = half_w; i < n_ang - half_w; ++i)
            {
                if (st[i] < search_lo || st[i] > search_hi) continue;
                const double before = w2_median(std::vector<double>(sa.begin() + std::max(0, i - half_w), sa.begin() + i));
                const double after  = w2_median(std::vector<double>(sa.begin() + i, sa.begin() + std::min(n_ang, i + half_w)));
                double diff = std::fabs(after - before);
                if (diff > 180.0) diff = 360.0 - diff;
                if (diff > best_shift && diff >= 30.0) { best_shift = diff; best_split_t = st[i]; }
            }

            bool split_done = false;
            if (!std::isnan(best_split_t))
            {
                int arg = 0;
                double bestd = 1e18;
                for (int j = 0; j < sub_n; ++j)
                {
                    const double d = std::fabs(sub[j].t - best_split_t);
                    if (d < bestd) { bestd = d; arg = j; }
                }
                const int split_idx = a + arg;     /* a + 全子段 argmin, 与脚本行为保持一致 */
                if (split_idx > a + 8 && split_idx < b - 8)
                {
                    const double d1 = sub[split_idx].t - sub[a].t;
                    const double d2 = sub[b].t - sub[split_idx].t;
                    if (d1 >= 0.5 && d2 >= 0.5)
                    {
                        all.push_back({ts_a + a, ts_a + split_idx, W2_LINEAR});
                        all.push_back({ts_a + split_idx, ts_a + b, W2_LINEAR});
                        split_done = true;
                    }
                }
            }
            if (!split_done) all.push_back({ts_a + a, ts_a + b, rs.phase});
        }
    }

    /* ---- ④ 排序 + 相邻同相短段合并(任一侧时长 <1s 即并入) ---- */
    std::sort(all.begin(), all.end(), [](const W2Seg &x, const W2Seg &y) { return x.a < y.a; });
    std::vector<W2Seg> merged;
    for (const auto &sgm : all)
    {
        const double dur = pts[sgm.b].t - pts[sgm.a].t;
        if (!merged.empty() && merged.back().phase == sgm.phase &&
            std::min(dur, pts[merged.back().b].t - pts[merged.back().a].t) < 1.0)
            merged.back().b = sgm.b;
        else
            merged.push_back(sgm);
    }

    /* ---- ⑤ 边界 spiral 复判: a)往复直线 lin>0.70 且折返率>0.5; b)平推扫掠 lin>0.55
     * 且局部角度标准差<40° 且平均步长>10px → 改判 linear ---- */
    for (auto &sgm : merged)
    {
        if (sgm.phase != W2_SPIRAL) continue;
        const int m = sgm.b - sgm.a + 1;
        if (m < 5) continue;
        const W2Point *sp = pts.data() + sgm.a;

        double cx = 0, cy = 0;
        for (int i = 0; i < m; ++i) { cx += sp[i].x; cy += sp[i].y; }
        cx /= m; cy /= m;
        double sxx = 0, syy = 0, sxy = 0;
        for (int i = 0; i < m; ++i)
        {
            const double u = sp[i].x - cx, v = sp[i].y - cy;
            sxx += u * u; syy += v * v; sxy += u * v;
        }
        sxx /= m; syy /= m; sxy /= m;
        const double trace = sxx + syy;
        if (trace <= 1.0) continue;
        const double disc = std::max(0.0, (sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy);
        const double lin = ((trace + std::sqrt(disc)) / 2.0) / trace;

        const std::vector<double> r2 =
            w2_reversal_rate(sp, m, std::min(1.5, pts[sgm.b].t - pts[sgm.a].t));
        double rsum = 0;
        int rcnt = 0;
        for (double v : r2)
            if (!std::isnan(v)) { rsum += v; ++rcnt; }
        const double avg_r = rcnt ? rsum / rcnt : 0.0;

        double ang_std = 999.0, mean_step = 0.0;
        if (m >= 8)
        {
            std::vector<double> angs, steps;
            for (int i = 1; i < m; ++i)
            {
                const double dx = sp[i].x - sp[i - 1].x, dy = sp[i].y - sp[i - 1].y;
                const double d = std::hypot(dx, dy);
                if (d > 3.0)
                {
                    steps.push_back(d);
                    double adeg = std::atan2(dy, dx) * 180.0 / CV_PI;
                    while (adeg >= 90.0) adeg -= 180.0;
                    while (adeg < -90.0) adeg += 180.0;
                    angs.push_back(adeg);
                }
            }
            if (!angs.empty())
            {
                double am = 0;
                for (double v : angs) am += v;
                am /= angs.size();
                double var = 0;
                for (double v : angs) var += (v - am) * (v - am);
                ang_std = std::sqrt(var / angs.size());
            }
            if (!steps.empty())
            {
                double sm = 0;
                for (double v : steps) sm += v;
                mean_step = sm / steps.size();
            }
        }
        if (lin > 0.70 && avg_r > 0.5) sgm.phase = W2_LINEAR;
        else if (lin > 0.55 && ang_std < 40.0 && mean_step > 10.0) sgm.phase = W2_LINEAR;
    }
    return merged;
}

/* 0°/180° 周期上的角度 EMA 平滑(同脚本 _smooth_angle_deg) */
static double w2_smooth_angle(double prev, double cur, double alpha)
{
    double a = cur;
    while (a - prev > 90.0)  a -= 180.0;
    while (a - prev < -90.0) a += 180.0;
    double r = std::fmod(alpha * a + (1.0 - alpha) * prev, 180.0);
    if (r < 0) r += 180.0;
    return r;
}

/* 晶圆椭圆 5 步提取 + 面积突变剔除 + EMA 平滑(同脚本 extract_wafer_ellipse)。
 * e 传入上次结果、传出更新值; 本帧提取失败时保持 e 不变。返回是否已有可用椭圆。 */
static bool w2_extract_ellipse(const cv::Mat &frame, const cv::Rect &bbox,
                               int dark_threshold, double alpha,
                               bool have_last, W2Ellipse &e)
{
    const int W = frame.cols, H = frame.rows, pad = 10;
    const int x1 = std::max(0, bbox.x - pad);
    const int y1 = std::max(0, bbox.y - pad);
    const int x2 = std::min(W, bbox.x + bbox.width + pad);
    const int y2 = std::min(H, bbox.y + bbox.height + pad);
    if (x2 <= x1 || y2 <= y1) return have_last;

    cv::Mat gray, blur, th;
    cv::cvtColor(frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)), gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blur, cv::Size(5, 5), 0);
    cv::threshold(blur, th, dark_threshold, 255, cv::THRESH_BINARY_INV);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(7, 7));
    cv::morphologyEx(th, th, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(th, th, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(th, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<cv::Point> all_pts;                /* 暗区碎片合并(回填全图坐标) */
    for (const auto &c : contours)
        if (cv::contourArea(c) > 100.0)
            for (const auto &q : c) all_pts.emplace_back(q.x + x1, q.y + y1);
    if ((int)all_pts.size() < 5) return have_last;

    std::vector<cv::Point> hull;                   /* 凸包桥接指缝遮挡 */
    cv::convexHull(all_pts, hull);
    if ((int)hull.size() < 5) return have_last;

    cv::RotatedRect fit;
    try { fit = cv::fitEllipse(hull); }
    catch (const cv::Exception &) { return have_last; }

    const W2Ellipse cur{fit.center.x, fit.center.y, fit.size.width, fit.size.height, fit.angle};
    if (!have_last) { e = cur; return true; }

    const double cur_area  = std::max(cur.a, 0.0) * std::max(cur.b, 0.0);
    const double prev_area = std::max(e.a, 0.0) * std::max(e.b, 0.0);
    if (prev_area > 0 && (cur_area / prev_area > 1.3 || cur_area / prev_area < 0.7))
        return true;                               /* 面积突变 → 拒绝, 保留上一帧 */

    e.angle = w2_smooth_angle(e.angle, cur.angle, alpha);
    e.cx = alpha * cur.cx + (1.0 - alpha) * e.cx;
    e.cy = alpha * cur.cy + (1.0 - alpha) * e.cy;
    e.a  = alpha * cur.a  + (1.0 - alpha) * e.a;
    e.b  = alpha * cur.b  + (1.0 - alpha) * e.b;
    return true;
}

struct Wafer2State
{
    /* 轨迹(仅检出帧; t=距本轮起点秒) */
    std::vector<W2Point> pts;
    bool     active = false;
    uint64_t t0_ms = 0;
    uint64_t last_seen_ms = 0;

    /* 晶圆椭圆(EMA 平滑, 同时充当覆盖基准) */
    bool      have_ellipse = false;
    W2Ellipse ellipse;
    bool      have_bbox = false;
    cv::Rect  last_bbox;

    /* 覆盖率: 归一化掩码 + 单位圆基准 */
    cv::Mat   cov_mask, unit_mask;
    int       wafer_pixels = 0;
    bool      have_prev_mpt = false;
    cv::Point prev_mpt;
    float     coverage = 0.0f;

    /* 分段缓存(每加 N 点重算一次) */
    std::vector<W2Seg> segs;
    std::vector<int>   seg_of;                     /* 点下标 → 段下标, -1=未入段 */
    int                pts_at_seg = 0;
    int                dbg_seg_cnt = 0;
};

static void logic_wafer2(ChannelContext *ctx)
{
    if (!ctx || !ctx->results) return;
    if (!ctx->frame || ctx->frame->empty())
    {
        draw_text(ctx, "wafer2: frame empty", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    if (!*ctx->state) *ctx->state = std::make_shared<Wafer2State>();
    auto &s = *std::static_pointer_cast<Wafer2State>(*ctx->state);

    /* ---- 参数(脚本默认值/config.yaml, 暂写死; 出处见函数头注释) ---- */
    constexpr int      DARK_THRESHOLD = 105;       /* --dark-thresh 默认 */
    constexpr double   EMA_ALPHA      = 0.05;      /* config.yaml roi.smooth_alpha */
    constexpr double   TRAIL_SEC      = 3.0;       /* --trail-sec 默认 */
    constexpr double   SCRAPER_W_PX   = 16.0;      /* 覆盖计算固定刮片宽度(模型坐标 px) */
    constexpr int      MASK_RES       = 500;       /* 归一化覆盖掩码分辨率 */
    constexpr uint64_t RESET_MS       = 5000;      /* 刮片消失超此时长 → 一轮结束复位 */
    constexpr int      MAX_PTS        = 4000;      /* 轨迹点上限(防循环流无限增长) */
    constexpr int      SEG_EVERY_PTS  = 5;         /* 每新增 N 点重算一次分段 */

    static const cv::Scalar W2_COLORS[5] = {       /* 分段调色板(BGR), 同脚本 SEGMENT_COLORS */
        cv::Scalar(0, 255, 0),   cv::Scalar(255, 165, 0), cv::Scalar(255, 0, 255),
        cv::Scalar(0, 255, 255), cv::Scalar(255, 80, 80),
    };

    const uint64_t now = ctx->timestamp_ms;

    if (s.cov_mask.empty())                        /* 掩码/单位圆惰性初始化 */
    {
        s.cov_mask  = cv::Mat::zeros(MASK_RES, MASK_RES, CV_8UC1);
        s.unit_mask = cv::Mat::zeros(MASK_RES, MASK_RES, CV_8UC1);
        cv::circle(s.unit_mask, cv::Point(MASK_RES / 2, MASK_RES / 2), MASK_RES / 2, cv::Scalar(255), -1);
        s.wafer_pixels = cv::countNonZero(s.unit_mask);
    }

    /* ---- ① 最高分 刮片(class 0) / 晶圆(class 1) ---- */
    AlgoResult *scraper = nullptr, *wafer = nullptr;
    for (auto &r : *ctx->results)
    {
        if (r.class_id == 0) { if (!scraper || r.score > scraper->score) scraper = &r; }
        else if (r.class_id == 1) { if (!wafer || r.score > wafer->score) wafer = &r; }
    }
    if (scraper) scraper->box_color = cv::Scalar(0, 255, 0);
    if (wafer)   wafer->box_color   = cv::Scalar(160, 160, 160);

    /* ---- ② 晶圆椭圆(当前框, 缺检沿用上次框, 同脚本 current or last_wafer_bbox) ---- */
    if (wafer) { s.last_bbox = wafer->box; s.have_bbox = true; }
    if (s.have_bbox)
        s.have_ellipse = w2_extract_ellipse(*ctx->frame, s.last_bbox, DARK_THRESHOLD,
                                            EMA_ALPHA, s.have_ellipse, s.ellipse);

    /* ---- ③ 一轮复位 ---- */
    if (s.active && !scraper && now - s.last_seen_ms > RESET_MS)
    {
        s.pts.clear(); s.segs.clear(); s.seg_of.clear();
        s.pts_at_seg = 0;
        s.cov_mask.setTo(0);
        s.have_prev_mpt = false;
        s.coverage = 0.0f;
        s.active = false;
    }

    /* ---- ④ 轨迹追加 + 覆盖累计(圆点 r=w/3 + 连线 thick=0.7w, 同脚本) ---- */
    if (scraper)
    {
        if (!s.active) { s.active = true; s.t0_ms = now; }
        s.last_seen_ms = now;
        if ((int)s.pts.size() < MAX_PTS)
        {
            const cv::Point c = scraper->box_center();
            s.pts.push_back({(float)c.x, (float)c.y, (double)(now - s.t0_ms) / 1000.0});

            if (s.have_ellipse)
            {
                const double rx = std::max(s.ellipse.a / 2.0, 1.0);
                const double ry = std::max(s.ellipse.b / 2.0, 1.0);
                const int mx = std::min(std::max((int)((c.x - s.ellipse.cx) / rx * (MASK_RES / 2.0) + MASK_RES / 2.0), 0), MASK_RES - 1);
                const int my = std::min(std::max((int)((c.y - s.ellipse.cy) / ry * (MASK_RES / 2.0) + MASK_RES / 2.0), 0), MASK_RES - 1);
                const cv::Point mpt(mx, my);
                const double w_mask = SCRAPER_W_PX / rx * (MASK_RES / 2.0);
                cv::circle(s.cov_mask, mpt, std::max(1, (int)(w_mask / 3.0)), cv::Scalar(255), -1);
                if (s.have_prev_mpt)
                    cv::line(s.cov_mask, s.prev_mpt, mpt, cv::Scalar(255), std::max(1, (int)(w_mask * 0.7)));
                s.prev_mpt = mpt;
                s.have_prev_mpt = true;

                cv::Mat inter;
                cv::bitwise_and(s.cov_mask, s.unit_mask, inter);
                s.coverage = (float)cv::countNonZero(inter) / (float)std::max(s.wafer_pixels, 1);
            }
        }
    }

    /* ---- ⑤ 分段(节流重算; 脚本为离线一次性) ---- */
    if ((int)s.pts.size() >= 2 &&
        (s.segs.empty() || (int)s.pts.size() - s.pts_at_seg >= SEG_EVERY_PTS))
    {
        s.segs = w2_segment_trajectory(s.pts);
        s.seg_of.assign(s.pts.size(), -1);
        for (int k = 0; k < (int)s.segs.size(); ++k)
            for (int i = s.segs[k].a; i <= s.segs[k].b && i < (int)s.seg_of.size(); ++i)
                s.seg_of[i] = k;
        s.pts_at_seg = (int)s.pts.size();

        if ((int)s.segs.size() != s.dbg_seg_cnt)
        {
            s.dbg_seg_cnt = (int)s.segs.size();
            printf("[wafer2] ch=%d pts=%zu segs=%d cov=%.1f%%\n",
                   ctx->chnId, s.pts.size(), s.dbg_seg_cnt, s.coverage * 100.0f);
        }
    }

    /* 节流重算间隙新加的点暂按最后一段着色, 避免当前点/轨迹闪烁 */
    auto seg_idx_of = [&](int i) -> int {
        if (i >= 0 && i < (int)s.seg_of.size()) return s.seg_of[i];
        return s.segs.empty() ? -1 : (int)s.segs.size() - 1;
    };

    /* ---- ⑥ 叠加绘制(顺序同脚本: 椭圆 → 覆盖 → 当前点 → 轨迹 → 图例) ---- */
    if (s.have_ellipse)
    {
        std::vector<cv::Point> poly;                /* 红色虚线椭圆: 隔段画线 */
        cv::ellipse2Poly(cv::Point((int)std::lround(s.ellipse.cx), (int)std::lround(s.ellipse.cy)),
                         cv::Size((int)std::lround(s.ellipse.a / 2.0), (int)std::lround(s.ellipse.b / 2.0)),
                         (int)std::lround(s.ellipse.angle), 0, 360, 5, poly);
        for (size_t k = 0; k + 1 < poly.size(); k += 2)
            draw_line(ctx, poly[k], poly[k + 1], cv::Scalar(0, 0, 255), 2);
        draw_text(ctx, "覆盖基准",
                  cv::Point((int)std::lround(s.ellipse.cx - s.ellipse.a / 2.0),
                            std::max(15, (int)std::lround(s.ellipse.cy + s.ellipse.b / 2.0) + 18)),
                  cv::Scalar(0, 0, 255), 0.4, 1);
    }
    else
        draw_text(ctx, "wafer2: waiting for wafer", cv::Point(20, 64), cv::Scalar(0, 165, 255), 0.5, 1);

    if (s.pts.size() >= 2)                          /* 覆盖带: 全程轨迹半透明绿粗折线 */
    {
        std::vector<cv::Point> tp;
        tp.reserve(s.pts.size());
        for (const auto &q : s.pts) tp.emplace_back((int)q.x, (int)q.y);
        draw_polyline(ctx, tp, cv::Scalar(0, 200, 0), (int)SCRAPER_W_PX, 0.25, false);
    }

    const double t_now = s.active ? (double)(now - s.t0_ms) / 1000.0 : 0.0;

    if (scraper && !s.pts.empty())                  /* 当前点: 段色实心圆 + 白圈 */
    {
        const int k = seg_idx_of((int)s.pts.size() - 1);
        if (k >= 0)
        {
            const cv::Point dot((int)s.pts.back().x, (int)s.pts.back().y);
            draw_circle(ctx, dot, 7, W2_COLORS[k % 5], -1);
            draw_circle(ctx, dot, 9, cv::Scalar(255, 255, 255), 1);
        }
    }

    for (int i = (int)s.pts.size() - 1; i >= 1; --i)   /* 最近 TRAIL_SEC 秒分段轨迹 */
    {
        if (s.pts[i].t < t_now - TRAIL_SEC) break;
        const int k = seg_idx_of(i);
        if (k < 0 || k != seg_idx_of(i - 1)) continue; /* 跨段不连线(同脚本) */
        draw_line(ctx, cv::Point((int)s.pts[i - 1].x, (int)s.pts[i - 1].y),
                  cv::Point((int)s.pts[i].x, (int)s.pts[i].y), W2_COLORS[k % 5], 2);
    }

    /* ---- ⑦ 顶部图例(单行, 覆盖/宽度 + 横擦/竖擦/环擦) ---- */
    const cv::Scalar ORANGE(0, 80, 230), GREEN(0, 160, 0);
    char buf[64];

    draw_text(ctx, "覆盖:", cv::Point(5, 18), ORANGE, 0.5, 2);
    snprintf(buf, sizeof(buf), "%.1f%%", s.coverage * 100.0f);
    draw_text(ctx, buf, cv::Point(52, 18), GREEN, 0.5, 2);
    draw_text(ctx, "宽:", cv::Point(102, 18), ORANGE, 0.5, 2);
    snprintf(buf, sizeof(buf), "%.0fpx", SCRAPER_W_PX);
    draw_text(ctx, buf, cv::Point(122, 18), GREEN, 0.5, 2);

    /* 模板槽位(同脚本 TEMPLATE): 检出的 linear 段依次填 横擦/竖擦, spiral 段填 环擦 */
    struct Slot { const char *name; bool detected; double t0, t1; };
    Slot slots[3] = {{"横擦", false, 0, 0}, {"竖擦", false, 0, 0}, {"环擦", false, 0, 0}};
    {
        const W2Seg *lin[2] = {nullptr, nullptr};
        const W2Seg *spi = nullptr;
        for (const auto &sgm : s.segs)
        {
            if (sgm.phase == W2_LINEAR) { if (!lin[0]) lin[0] = &sgm; else if (!lin[1]) lin[1] = &sgm; }
            else if (sgm.phase == W2_SPIRAL) { if (!spi) spi = &sgm; }
        }
        const W2Seg *pick[3] = {lin[0], lin[1], spi};
        for (int i = 0; i < 3; ++i)
            if (pick[i])
            {
                slots[i].detected = true;
                slots[i].t0 = s.pts[pick[i]->a].t;
                slots[i].t1 = s.pts[pick[i]->b].t;
            }
    }
    /* 每个槽: label(~38px) + value(~60px) = ~98px; 3槽从 x=163 起 */
    static const int slot_x[3] = {163, 290, 417};
    for (int i = 0; i < 3; ++i)
    {
        snprintf(buf, sizeof(buf), "%s:", slots[i].name);
        draw_text(ctx, buf, cv::Point(slot_x[i], 18), ORANGE, 0.5, 2);
        if (slots[i].detected && t_now >= slots[i].t0 && t_now <= slots[i].t1)
            snprintf(buf, sizeof(buf), "%.1fs ▶", t_now - slots[i].t0);
        else if (slots[i].detected && t_now > slots[i].t1)
            snprintf(buf, sizeof(buf), "%.1fs ✓", slots[i].t1 - slots[i].t0);
        else
            snprintf(buf, sizeof(buf), "○");
        draw_text(ctx, buf, cv::Point(slot_x[i] + 38, 18), GREEN, 0.5, 2);
    }
}

REGISTER_LOGIC("logic_wafer2", logic_wafer2);
