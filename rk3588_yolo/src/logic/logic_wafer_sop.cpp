/* ============================================================================
 * @file logic_wafer_sop.cpp
 * logic_wafer_sop —— 晶圆湿法清洗 SOP 合规检测 (装晶圆的特氟龙花篮转移清洗)
 *
 * 全程"逐工序点亮"可视化完成度(不做结算): 左上一列清单, 每行一开始红色,
 * 达成即变绿。 默认 6 行(4 槽 + 抖动 + 朝向): 1..N  各槽(sop_sequence):
 * 花篮"真正进入"(持续 enter_ms 停留确认)该槽 → 该行变绿; 漏掉的槽保持红。 N+1
 * 抖动次数: 进入最后一个槽(纯水槽)后才计数, 跟踪框中心 Y 上下往复, 每达
 * sop_shake_amplitude 记一次; 次数 >= sop_shake_min_count → 变绿。 N+2   朝向:
 * 默认绿"方向未变化"; 类别(默认 bkt_normal/bkt_abnormal)持续 sop_dir_sec 翻转后
 *         → 红"方向变化"。
 * 另: 高亮花篮当前实际所在槽(粗框); 花篮框按已确认朝向上色;
 * 进入首槽=开新一轮(清空清单), 花篮长时间离场也会自动复位。
 *
 * 依赖: 本通道按 sop_sequence 各名字画好 ROI
 * 区域(ctx->rois)。纯画面提示(不上报)。 坐标系: 模型输入尺寸(640)。画面文字经
 * freetype 渲染, 直接显示中文(槽名/抖动次数/方向)。 可调参数见 logics.json。
 * ==========================================================================*/
#include "logic_common.h"

/* 逗号分隔字符串 → 去首尾空白的非空片段 */
static std::vector<std::string> sop_split_csv(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        size_t b = cur.find_first_not_of(" \t\r\n");
        size_t e = cur.find_last_not_of(" \t\r\n");
        if (b != std::string::npos)
            out.push_back(cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (char c : s)
    {
        if (c == ',')
            flush();
        else
            cur += c;
    }
    flush();
    return out;
}

struct WaferSopState
{
    int cur_zone = -1;        /* 最近"确认进入"的 seq 槽下标(去重用) */
    std::vector<int> visited; /* 已"确认进入"(点亮)的 seq 槽下标集合 */

    /* 进入区域防抖: 候选槽须持续 enter_ms 才确认进入 */
    int cand_zone = -2;         /* 当前帧所在槽(瞬时); -1=转移, -2=无花篮 */
    uint64_t cand_since_ms = 0; /* 候选槽起始时刻 */

    /* 朝向切换防抖: 新类别须持续 dir_ms 才确认 */
    int dir_cand = -1; /* 瞬时类别: 0=正常 1=翻转 -1=无花篮 */
    uint64_t dir_cand_since_ms = 0;
    int dir_confirmed = 0;      /* 已确认朝向: 0=正常 1=翻转 */
    bool reversed_seen = false; /* 本轮工序内确认出现过翻转 */

    /* 抖动(纯水槽=最后一个槽内) */
    bool in_shake = false;
    int shake_count = 0;
    float ext_y = -1.0f; /* 当前方向上的极值 Y */
    int y_dir = 0;       /* +1 向下 / -1 向上 / 0 未定 */

    uint64_t last_seen_ms = 0; /* 最近一次检测到花篮(任意位置); 长时间不见 → 复位本轮 */
};

static void logic_wafer_sop(ChannelContext *ctx)
{
    if (!ctx)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<WaferSopState>();
    auto &s = *std::static_pointer_cast<WaferSopState>(*ctx->state);

    /* ---- 参数(每帧现读, 支持网页热重载) ---- */
    const ChannelConfig *cfg = ctx->config;
    const std::vector<std::string> seq = sop_split_csv(cfg ? cfg->sop_sequence : std::string());
    const std::string normLab = cfg ? cfg->basket_normal_label : std::string("bkt_normal");
    const std::string abnLab = cfg ? cfg->basket_abnormal_label : std::string("bkt_abnormal");
    const int shakeAmp = cfg ? cfg->sop_shake_amplitude : 15;
    const int shakeMin = cfg ? cfg->sop_shake_min_count : 3;
    const float enterSec = cfg ? cfg->sop_enter_sec : 0.8f; /* 进入区域确认时长 */
    const float dirSec = cfg ? cfg->sop_dir_sec : 0.5f;     /* 朝向切换确认时长 */
    const uint64_t enter_ms = (uint64_t)std::max(0, (int)(enterSec * 1000.0f + 0.5f));
    const uint64_t dir_ms = (uint64_t)std::max(0, (int)(dirSec * 1000.0f + 0.5f));
    const int nseq = (int)seq.size();
    const int lastIdx = nseq - 1;   /* 最后一个槽 = 纯水槽 = 抖动槽 */
    const int MOVE_EPS = 2;         /* Y 抖动死区(px) */
    const uint64_t RESET_MS = 6000; /* 花篮离场超此时长 → 清空本轮清单(非结算, 仅复位) */

    if (nseq < 2)
    {
        draw_text(ctx, "wafer_sop: set 'sop_sequence' + draw named ROI zones", cv::Point(20, 30),
                  cv::Scalar(0, 165, 255), 0.55, 2);
        return;
    }

    /* ---- 找"花篮"(置信度最高的 normal/abnormal 检测) ---- */
    AlgoResult *basket = nullptr;
    bool basket_reversed = false;
    if (ctx->results)
    {
        float best = -1.0f;
        for (auto &r : *ctx->results)
        {
            const bool isNorm = (r.label == normLab);
            const bool isAbn = (r.label == abnLab);
            if (!isNorm && !isAbn)
                continue;
            if (r.score > best)
            {
                best = r.score;
                basket = &r;
                basket_reversed = isAbn;
            }
        }
    }

    /* ---- 花篮中心落在哪个 seq 槽(命名区域) ---- */
    cv::Point center(-1, -1);
    int zoneNow = -1;
    if (basket)
    {
        center = basket->box_center();
        for (int i = 0; i < nseq; ++i)
        {
            const RoiZone *z = ctx->roi_by_name(seq[i].c_str());
            if (z && z->polygon.size() >= 3 && cv::pointPolygonTest(z->polygon, center, false) >= 0.0)
            {
                zoneNow = i;
                break;
            }
        }
    }

    const uint64_t now = ctx->timestamp_ms;

    /* 花篮长时间离场 → 清空本轮清单(无结算, 仅为下一篮复位) */
    if (basket)
        s.last_seen_ms = now;
    else if (s.last_seen_ms != 0 && (now - s.last_seen_ms) > RESET_MS)
        s = WaferSopState();

    /* ---- ② 朝向(类别)防抖: 新类别须持续 dir_ms 才确认翻转/恢复, 滤除瞬时误检
     * ---- */
    if (basket)
    {
        const int cls = basket_reversed ? 1 : 0;
        if (cls != s.dir_cand)
        {
            s.dir_cand = cls;
            s.dir_cand_since_ms = now;
        }
        if (s.dir_cand != s.dir_confirmed && (now - s.dir_cand_since_ms) >= dir_ms)
        {
            s.dir_confirmed = s.dir_cand;
            if (s.dir_confirmed == 1)
                s.reversed_seen = true;
        }
    }
    else
        s.dir_cand = -1; /* 花篮消失: 重置, 重新出现需重新计时 */

    const bool revConfirmed = (s.dir_confirmed == 1);
    const bool revPending = basket && (s.dir_cand != s.dir_confirmed);

    /* ---- ① 进入区域防抖 + 逐槽点亮(完成度) ----
     * 花篮中心须在同一 seq 槽内持续 enter_ms 才算"真正进入"(滤除转移时短暂划过);
     * 一旦确认进入某槽就把它点亮(visited)。不做结算:
     * 完成度全程实时反映在左上清单。 进入首槽(去胶槽1)视为开新一轮 →
     * 先清空上一篮的清单。*/
    if (!basket)
        s.cand_zone = -2;
    else if (zoneNow != s.cand_zone)
    {
        s.cand_zone = zoneNow;
        s.cand_since_ms = now;
    }

    if (basket && s.cand_zone >= 0 && s.cand_zone != s.cur_zone && (now - s.cand_since_ms) >= enter_ms)
    {
        const int z = s.cand_zone;
        if (z == 0) /* 进入首槽 = 开新一轮, 清空清单 */
        {
            const uint64_t seen = s.last_seen_ms;
            s = WaferSopState();
            s.last_seen_ms = seen;
            s.cand_zone = 0;
            s.cand_since_ms = now;
        }
        if (std::find(s.visited.begin(), s.visited.end(), z) == s.visited.end())
            s.visited.push_back(z); /* 点亮该槽 */
        s.cur_zone = z;

        if (z == lastIdx && !s.in_shake) /* 确认进入纯水槽 → 开始抖动计数 */
        {
            s.in_shake = true;
            s.shake_count = 0;
            s.ext_y = (float)center.y;
            s.y_dir = 0;
        }
        if (z != lastIdx)
            s.in_shake = false;
    }

    /* ---- ③ 抖动检测(纯水槽内, 跟踪框中心 Y 的上下往复) ---- */
    if (s.in_shake && basket && zoneNow == lastIdx)
    {
        const float y = (float)center.y;
        if (s.y_dir == 0)
        {
            if (y - s.ext_y >= MOVE_EPS)
            {
                s.y_dir = +1;
                s.ext_y = y;
            }
            else if (s.ext_y - y >= MOVE_EPS)
            {
                s.y_dir = -1;
                s.ext_y = y;
            }
        }
        else if (s.y_dir > 0) /* 正在向下(Y 增大) */
        {
            if (y > s.ext_y)
                s.ext_y = y; /* 继续探底 */
            else if (s.ext_y - y >= (float)shakeAmp)
            {
                s.shake_count++;
                s.y_dir = -1;
                s.ext_y = y;
            }
        }
        else /* 正在向上(Y 减小) */
        {
            if (y < s.ext_y)
                s.ext_y = y;
            else if (y - s.ext_y >= (float)shakeAmp)
            {
                s.shake_count++;
                s.y_dir = +1;
                s.ext_y = y;
            }
        }
    }

    /* 某 seq 槽是否已"确认进入"(点亮) */
    auto visited_has = [&](int i) { return std::find(s.visited.begin(), s.visited.end(), i) != s.visited.end(); };

    /* ===================== 画面提示(全 ASCII) ===================== */
    /* 1) 各 seq 槽多边形: 已点亮=绿 / 未到=黄;
     * 花篮当前实际所在槽=粗框。首顶点旁标 ASCII tag */
    for (int i = 0; i < nseq; ++i)
    {
        const RoiZone *z = ctx->roi_by_name(seq[i].c_str());
        if (!z || z->polygon.size() < 3)
            continue;
        const bool isCur = (i == zoneNow);
        const cv::Scalar col = visited_has(i) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 255, 255);
        draw_polyline(ctx, z->polygon, col, isCur ? 4 : 2, 1.0, true);
        const cv::Point a(z->polygon.front().x + 4, std::max(14, z->polygon.front().y - 6));
        draw_text(ctx, seq[i].c_str(), a, col, 0.5, 1); /* 直接用真实槽名(中文) */
    }

    /* 2) 花篮框/中心点: 按"已确认朝向"上色(正常绿/翻转红) */
    if (basket)
    {
        const cv::Scalar bcol = revConfirmed ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
        basket->box_color = bcol;
        draw_circle(ctx, center, 4, bcol, 2);
    }

    /* 3) 左上"工序完成度"清单: 每行 红=未达成 / 绿=已达成, 逐工序点亮(全程实时,
     * 无结算) */
    const cv::Scalar GREEN(0, 238, 0), RED(0, 0, 230); /* 深绿 / 深红, 不刺眼 */
    int yrow = 28;
    char line[96];
    /* 3.1 各槽: "真正进入"即点亮; 漏掉/未到的保持红 */
    for (int i = 0; i < nseq; ++i)
    {
        snprintf(line, sizeof(line), "%d. %s", i + 1, seq[i].c_str());
        draw_text(ctx, line, cv::Point(20, yrow), visited_has(i) ? GREEN : RED, 0.6, 2);
        yrow += 28;
    }
    /* 3.2 抖动次数(进入纯水槽后才计数): 达到阈值点亮 */
    snprintf(line, sizeof(line), "%d. 抖动次数 %d/%d", nseq + 1, s.shake_count, shakeMin);
    draw_text(ctx, line, cv::Point(20, yrow), (s.shake_count >= shakeMin) ? GREEN : RED, 0.6, 2);
    yrow += 28;
    /* 3.3 朝向: 未变化=绿 / 变化=红 */
    snprintf(line, sizeof(line), "%d. %s%s", nseq + 2, s.reversed_seen ? "方向变化" : "方向未变化",
             revPending ? "(确认中)" : "");
    draw_text(ctx, line, cv::Point(20, yrow), s.reversed_seen ? RED : GREEN, 0.6, 2);
}

REGISTER_LOGIC("logic_wafer_sop", logic_wafer_sop);
