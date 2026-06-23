/* ============================================================================
 * @file logic_path_sop.cpp
 * logic_path_sop —— 目标 SOP 路径合规检测 (单目标·按类别·严格顺序; 仅屏幕报警, 不上报)
 *
 * 目标必须【严格按配置的顺序】依次进入各区域。两类报警(均只在屏幕上红字显示, 不上报服务器):
 *   ① 顺序错误: 目标进入了"设计内区域", 但不是当前期望的下一步 → 即时报警(不必等结束)。
 *   ② 漏掉区域: 工序结束时, 应进区域里还有从没进过的 → 结算报警(必须等工序结束才能判)。
 *
 * 何时算"工序结束"(决定漏检何时结算), 由 path_end_mode 选:
 *   - "leave"  (离场超时): 目标持续检测不到超过 path_reset_sec 秒 → 工序结束。
 *   - "endzone"(终点区域): 目标进入 path_end_zone 指定的区域 → 工序结束;
 *                          目标中途离场超过 path_reset_sec 秒 → 兜底也判结束。
 *   另: 按序走完全部步骤(成功) 也视为结束。结束后目标再次进入"第1步区域"= 开新一轮。
 *
 * 配置(每帧热读, 由网页 SOP 流程画布生成):
 *   path_sequence      步骤序列: 逗号分隔区域名(可重复=多次进入), 顺序=严格期望顺序
 *   path_target_label  目标类别名
 *   path_enter_list    每步进入确认(秒), 与 path_sequence 对齐(空项回退 path_enter_sec)
 *   path_dwell_list    每步最小停留(秒), 与 path_sequence 对齐(空项回退 path_dwell_min_sec; 仅显示提示)
 *   path_reset_sec     离场超时(秒): 工序结束确认时长
 *   path_end_mode      "leave" | "endzone"
 *   path_end_zone      终点区域名(endzone 模式)
 * 坐标=模型输入尺寸; 中文经 freetype 直接显示。
 * ==========================================================================*/
#include "logic_common.h"

/* 逗号分隔 → 去首尾空白的非空片段 */
static std::vector<std::string> path_split_csv(const std::string &s)
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

/* 按 ',' 拆分但保留空字段(与 path_sequence 对齐的 per-step 列表用) */
static std::vector<std::string> path_split_keep(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == ',')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

static float path_parse_f(const std::string &s, float dflt)
{
    float v = dflt;
    return (sscanf(s.c_str(), "%f", &v) == 1) ? v : dflt;
}

struct PathSopState
{
    int expect = 0;        /* 下一个期望进入的步骤下标(0..nseq) */
    int cur_step = -1;     /* 最近按序进入的步骤(停留计时用) */
    std::string cur_zone;  /* 最近确认所在区域名("" = 不在任何区域) */
    std::string cand_zone; /* 候选区域(进入防抖) */
    uint64_t cand_since_ms = 0;
    std::vector<std::string> visited; /* 进过的区域名(任意顺序; 用于漏检) */
    std::vector<uint64_t> dwell_ms;
    bool order_error = false;        /* 顺序错误(latch) */
    std::string err_detail;          /* 顺序错误详情: 进了X 应进Y */
    bool ended = false;              /* 本轮工序已结束(显示结算) */
    std::vector<std::string> missed; /* 结束时算出的漏掉区域 */
    bool completed = false;          /* 严格按序走完全部步骤 */
    uint64_t last_seen_ms = 0;
};

static void logic_path_sop(ChannelContext *ctx)
{
    if (!ctx)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<PathSopState>();
    auto &s = *std::static_pointer_cast<PathSopState>(*ctx->state);

    /* ---- 参数 ---- */
    const ChannelConfig *cfg = ctx->config;
    const std::vector<std::string> seq = path_split_csv(cfg ? cfg->path_sequence : std::string());
    const std::string targetLab = cfg ? cfg->path_target_label : std::string();
    const float endSec = cfg ? cfg->path_reset_sec : 5.0f;
    const uint64_t end_ms = (uint64_t)std::max(0, (int)(endSec * 1000.0f + 0.5f));
    const std::string endMode = cfg ? cfg->path_end_mode : std::string("leave");
    const std::string endZone = cfg ? cfg->path_end_zone : std::string();
    const bool useEndZone = (endMode == "endzone") && !endZone.empty();
    const bool doReport = cfg ? cfg->path_report : false; /* 连了"上报配置"节点才上报 */
    const int nseq = (int)seq.size();

    if (nseq < 1 || targetLab.empty())
    {
        draw_text(ctx, "path_sop: 设置 path_sequence + 目标类别, 并画好同名 ROI 区域", cv::Point(20, 30),
                  cv::Scalar(0, 165, 255), 0.55, 2);
        return;
    }
    if ((int)s.dwell_ms.size() != nseq)
        s.dwell_ms.assign(nseq, 0);

    /* ---- 每步进入确认/最小停留(对齐 path_sequence) ---- */
    const std::vector<std::string> enterList = path_split_keep(cfg ? cfg->path_enter_list : std::string());
    const std::vector<std::string> dwellList = path_split_keep(cfg ? cfg->path_dwell_list : std::string());
    const float enterDflt = cfg ? cfg->path_enter_sec : 0.5f;
    const float dwellDflt = cfg ? cfg->path_dwell_min_sec : 0.0f;
    std::vector<uint64_t> enter_ms(nseq);
    std::vector<float> dwell_min(nseq);
    for (int i = 0; i < nseq; ++i)
    {
        const float e = (i < (int)enterList.size()) ? path_parse_f(enterList[i], enterDflt) : enterDflt;
        const float d = (i < (int)dwellList.size()) ? path_parse_f(dwellList[i], dwellDflt) : dwellDflt;
        enter_ms[i] = (uint64_t)std::max(0, (int)(e * 1000.0f + 0.5f));
        dwell_min[i] = std::max(0.0f, d);
    }

    /* 应进区域(去重, 漏检基准) */
    std::vector<std::string> required;
    for (const auto &z : seq)
        if (std::find(required.begin(), required.end(), z) == required.end())
            required.push_back(z);

    /* ---- 目标 + 所在区域 ---- */
    AlgoResult *target = nullptr;
    if (ctx->results)
    {
        float best = -1.0f;
        for (auto &r : *ctx->results)
            if (r.label == targetLab && r.score > best)
            {
                best = r.score;
                target = &r;
            }
    }
    cv::Point center(-1, -1);
    if (target)
        center = target->box_center();

    std::string inZone;
    if (target && ctx->rois)
        for (const auto &z : *ctx->rois)
            if (z.polygon.size() >= 3 && cv::pointPolygonTest(z.polygon, center, false) >= 0.0)
            {
                inZone = z.name;
                break;
            }

    auto isDesigned = [&](const std::string &nm) { return std::find(seq.begin(), seq.end(), nm) != seq.end(); };
    auto inVisited = [&](const std::string &nm) {
        return std::find(s.visited.begin(), s.visited.end(), nm) != s.visited.end();
    };

    /* 上报一次(仅当画布连了"上报配置"节点 → doReport)。SOP 事件稀疏, 不做时间冷却:
     * 顺序错误本轮 latch 一次、漏检结束时算一次, 各只上报一次; 开新一轮自然解锁、可再报。
     * server_url 为空 = 用上报服务默认地址。发当时的模型输入帧快照。*/
    auto report = [&](const char *atype) {
        if (!ctx->frame || ctx->frame->empty())
            return;
        const char *url = (cfg && !cfg->server_url.empty()) ? cfg->server_url.c_str() : nullptr;
        /* doReport = 画布是否连了 server 型"上报配置"节点(path_report); false 时 enqueue 内部跳过 */
        alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, atype, doReport, url);
    };
    auto settle = [&]() { /* 工序结束 → 结算漏检 */
                          s.missed.clear();
                          for (const auto &rq : required)
                              if (!inVisited(rq))
                                  s.missed.push_back(rq);
                          s.ended = true;
                          if (!s.missed.empty())
                              report("sop_missed"); /* 漏检: 上报一次 */
    };
    auto start_round = [&]() { /* 开新一轮 */
                               s.expect = 0;
                               s.cur_step = -1;
                               s.cur_zone.clear();
                               s.cand_zone.clear();
                               s.visited.clear();
                               s.order_error = false;
                               s.err_detail.clear();
                               s.ended = false;
                               s.missed.clear();
                               s.completed = false;
                               std::fill(s.dwell_ms.begin(), s.dwell_ms.end(), 0);
    };

    const uint64_t now = ctx->timestamp_ms;
    if (target)
        s.last_seen_ms = now;

    /* ---- 工序结束①: 离场超时(leave 主判定 / endzone 兜底) ---- */
    if (!s.ended && s.last_seen_ms != 0 && !target && (now - s.last_seen_ms) > end_ms)
        settle();

    /* ---- 进入防抖 + 严格顺序 + 终点/新一轮 ---- */
    if (target)
    {
        if (inZone != s.cand_zone)
        {
            s.cand_zone = inZone;
            s.cand_since_ms = now;
        }
        const uint64_t confirm_ms = (s.expect < nseq) ? enter_ms[s.expect] : (uint64_t)500;
        if (!inZone.empty() && inZone != s.cur_zone && (now - s.cand_since_ms) >= confirm_ms)
        {
            /* 已结束又进"第1步区域" → 开新一轮 */
            if (s.ended && inZone == seq[0])
                start_round();

            if (!s.ended)
            {
                s.cur_zone = inZone;
                if (!inVisited(inZone))
                    s.visited.push_back(inZone);

                const std::string expectZone = (s.expect < nseq) ? seq[s.expect] : std::string();
                if (inZone == expectZone) /* 正确的下一步 */
                {
                    s.cur_step = s.expect;
                    s.expect++;
                    if (s.expect >= nseq)
                    {
                        s.completed = true;
                        settle();
                    } /* 按序走完 = 成功结束 */
                }
                else if (isDesigned(inZone)) /* 进了设计区域但不是期望的下一步 → 顺序错误 */
                {
                    if (!s.order_error)
                    {
                        s.err_detail =
                            "进了[" + inZone + "] 应进[" + (expectZone.empty() ? std::string("-") : expectZone) + "]";
                        report("sop_order_err"); /* 顺序错误: 上报一次(本轮) */
                    }
                    s.order_error = true;
                }
                /* 工序结束②: endzone 模式进入终点区域 */
                if (!s.ended && useEndZone && inZone == endZone)
                    settle();
            }
        }
    }

    /* 停留计时(显示用): 目标停在"当前步"区域内则累加该步 */
    if (target && !s.ended && s.cur_step >= 0 && s.cur_step < nseq && inZone == seq[s.cur_step])
        s.dwell_ms[s.cur_step] += (uint64_t)std::max(0.0f, ctx->dt_ms);

    /* ===================== 画面绘制 ===================== */
    const cv::Scalar GREEN(0, 238, 0), RED(0, 0, 230), ORANGE(0, 165, 255), GRAY(170, 170, 170), CYAN(255, 255, 0),
        WHITE(255, 255, 255), DGREEN(0, 180, 0);

    /* 1) 各区域: 目标所在=绿粗 / 期望下一步=青 / 已进=深绿 / 未到=黄; 终点区域标注 */
    const std::string expectZone = (s.expect < nseq && !s.ended) ? seq[s.expect] : std::string();
    if (ctx->rois)
        for (const auto &z : *ctx->rois)
        {
            if (z.polygon.size() < 3)
                continue;
            std::string refs;
            for (int i = 0; i < nseq; ++i)
                if (seq[i] == z.name)
                {
                    if (!refs.empty())
                        refs += ",";
                    refs += std::to_string(i + 1);
                }
            const bool hasTarget = target && cv::pointPolygonTest(z.polygon, center, false) >= 0.0;
            const bool isExpect = !expectZone.empty() && z.name == expectZone;
            const cv::Scalar col = hasTarget  ? GREEN
                                   : isExpect ? CYAN
                                              : (inVisited(z.name) ? DGREEN : cv::Scalar(0, 255, 255));
            draw_polyline(ctx, z.polygon, col, hasTarget ? 4 : (isExpect ? 3 : 2), 1.0, true);
            char tag[128];
            snprintf(tag, sizeof(tag), "%s%s 步%s", z.name.c_str(), (useEndZone && z.name == endZone) ? "[终点]" : "",
                     refs.empty() ? "-" : refs.c_str());
            const cv::Point a(z.polygon.front().x + 4, std::max(14, z.polygon.front().y - 6));
            draw_text(ctx, tag, a, col, 0.5, 1);
        }

    /* 2) 目标框/中心 */
    if (target)
    {
        target->box_color = cv::Scalar(0, 200, 0);
        draw_circle(ctx, center, 4, cv::Scalar(0, 200, 0), 2);
    }

    /* 3) 左上信息 + 报警(红字, 不上报) */
    int yrow = 28;
    char line[160];

    if (s.order_error) /* 顺序错误: 即时报警 */
    {
        draw_text(ctx, "报警: 顺序错误!", cv::Point(16, yrow), RED, 0.9, 3);
        yrow += 34;
        if (!s.err_detail.empty())
        {
            draw_text(ctx, s.err_detail.c_str(), cv::Point(16, yrow), RED, 0.55, 2);
            yrow += 26;
        }
    }
    if (s.ended && !s.missed.empty()) /* 漏掉区域: 结束时结算报警 */
    {
        std::string m;
        for (size_t k = 0; k < s.missed.size(); ++k)
        {
            if (k)
                m += ",";
            m += s.missed[k];
        }
        draw_text(ctx, "报警: 漏掉区域!", cv::Point(16, yrow), RED, 0.9, 3);
        yrow += 34;
        snprintf(line, sizeof(line), "未进: %s", m.c_str());
        draw_text(ctx, line, cv::Point(16, yrow), RED, 0.6, 2);
        yrow += 28;
    }
    if (s.ended && !s.order_error && s.missed.empty()) /* 合规 */
    {
        draw_text(ctx, "工序合规完成", cv::Point(16, yrow), GREEN, 0.8, 2);
        yrow += 32;
    }

    snprintf(line, sizeof(line), "目标[%s]: %s  当前区域: %s", targetLab.c_str(), target ? "在线" : "离线",
             inZone.empty() ? "--" : inZone.c_str());
    draw_text(ctx, line, cv::Point(16, yrow), target ? WHITE : GRAY, 0.5, 1);
    yrow += 22;

    snprintf(line, sizeof(line), "进度 %d/%d  结束方式: %s", s.expect, nseq, useEndZone ? "终点区域" : "离场超时");
    draw_text(ctx, line, cv::Point(16, yrow), s.ended ? GRAY : CYAN, 0.55, 1);
    yrow += 24;

    if (!s.ended && !expectZone.empty())
    {
        snprintf(line, sizeof(line), "下一步 第%d步: %s", s.expect + 1, expectZone.c_str());
        draw_text(ctx, line, cv::Point(16, yrow), CYAN, 0.5, 1);
        yrow += 22;
    }

    /* 逐步骤: 已完成(按序)=绿/橙 / 当前应进=青 / 未到=灰。
     * 严格顺序下"是否完成"只看步号(i < expect), 不看区域名是否进过 ——
     * 否则区域重复时(如 区域1 既是第1步又是第3步), 第一次进区域1 会把第3步误标成"已进"。*/
    const int hmax = (ctx->frame ? ctx->frame->rows : 640) - 16;
    for (int i = 0; i < nseq; ++i)
    {
        if (yrow > hmax)
        {
            draw_text(ctx, "...", cv::Point(16, yrow), GRAY, 0.5, 1);
            break;
        }
        const float dsec = s.dwell_ms[i] / 1000.0f;
        const bool done = i < s.expect;
        const bool cur = (i == s.expect) && !s.ended;
        cv::Scalar c;
        const char *mk;
        if (done)
        {
            const bool dwellOk = (dwell_min[i] <= 0.0f) || (dsec + 1e-3f >= dwell_min[i]);
            c = dwellOk ? GREEN : ORANGE;
            mk = dwellOk ? "已完成" : "停留不足";
        }
        else if (cur)
        {
            c = CYAN;
            mk = "当前应进";
        }
        else
        {
            c = GRAY;
            mk = "待经过";
        }
        snprintf(line, sizeof(line), "%d.%s 停留%.1fs %s", i + 1, seq[i].c_str(), dsec, mk);
        draw_text(ctx, line, cv::Point(16, yrow), c, 0.55, 1);
        yrow += 22;
    }
}

REGISTER_LOGIC("logic_path_sop", logic_path_sop);
