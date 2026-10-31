/*
 * 仅用 YOLOv8s 三类正类框：0 outfit / 1 helmet / 2 glove。
 * 设备公共 SDK 0.1.0，C++11；所有框和 ROI 均为 640x640 业务坐标。
 * 本模块输出疑似缺失，不把几何估计当成真实身体可见性或已确认违规。
 */
#include <channel.h>
#include <drawing.h>
#include <events.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Box = cv::Rect2f;
    enum Item
    {
        OUTFIT = 0,
        HELMET = 1,
        GLOVE = 2
    };
    struct Detection
    {
        Box box;
        int cls;
        int tracker;
        float score;
    };
    struct Candidate
    {
        Box head, body;
        std::array<int, 2> source{{-1, -1}};
        std::array<int, 2> tracker{{-1, -1}};
        bool ambiguous = false;
    };
    struct Sample
    {
        uint64_t time;
        bool missing;
    };
    struct Session
    {
        uint64_t id = 0, last_seen = 0, first_seen = 0, last_sample = 0, last_attempt = 0;
        Candidate anchor;
        std::array<std::deque<Sample>, 3> evidence;
        std::array<int, 3> status{{-1, -1, -1}}; // -1 unknown, 0 detected, 1 suspected missing
        unsigned confirmed_mask = 0;
        bool notified = false, sampled = false, attempted = false;
        uint64_t touched = 0;
        unsigned current_mask = 0;
        int glove_count = 0;
        std::array<std::string, 3> reason{{"unobserved", "unobserved", "unobserved"}};
        std::string event_id;
    };
    struct State
    {
        std::vector<Session> sessions;
        uint64_t next_id = 1, last_time = 0, epoch = 0, last_error = 0;
        int64_t last_frame = -1, last_result_frame = -1;
        std::string config_signature;
        bool clock_started = false, error_logged = false;
    };
    struct Config
    {
        float score, low_score, overlap, match_distance, missing_ratio;
        float head_scale, body_width_scale, body_height_scale, dedupe_iou;
        int model_index, min_samples, required_gloves, min_part_pixels, max_sessions;
        uint64_t sample_ms, window_ms, span_ms, warmup_ms, retention_ms, gap_ms;
        bool enabled, gloves_enabled, draw;
        std::string work_roi, block_prefix;
        std::array<std::string, 3> clear_roi;
    };

    Config config(ChannelContext *ctx)
    {
        Config c;
        c.score = ctx->param_float("confidence");
        c.dedupe_iou = ctx->param_float("dedupe_iou");
        c.low_score = std::min(c.score, ctx->param_float("ambiguity_confidence"));
        c.overlap = ctx->param_float("crowd_overlap");
        c.match_distance = ctx->param_float("match_distance");
        c.missing_ratio = ctx->param_float("missing_ratio");
        c.head_scale = ctx->param_float("head_to_outfit_width");
        c.body_width_scale = ctx->param_float("outfit_to_helmet_width");
        c.body_height_scale = ctx->param_float("outfit_height_ratio");
        c.model_index = static_cast<int>(ctx->param_int("model_index"));
        c.min_samples = static_cast<int>(ctx->param_int("min_samples"));
        c.required_gloves = static_cast<int>(ctx->param_int("required_gloves"));
        c.min_part_pixels = static_cast<int>(ctx->param_int("min_part_pixels"));
        c.max_sessions = static_cast<int>(ctx->param_int("max_sessions"));
        c.sample_ms = static_cast<uint64_t>(ctx->param_int("sample_interval_ms"));
        c.window_ms = static_cast<uint64_t>(ctx->param_int("evidence_window_ms"));
        c.span_ms = static_cast<uint64_t>(ctx->param_int("min_evidence_span_ms"));
        c.warmup_ms = static_cast<uint64_t>(ctx->param_int("warmup_ms"));
        c.retention_ms = static_cast<uint64_t>(ctx->param_int("session_retention_ms"));
        c.gap_ms = static_cast<uint64_t>(ctx->param_int("max_frame_gap_ms"));
        c.enabled = ctx->param_bool("alarm_enabled");
        c.gloves_enabled = ctx->param_bool("glove_check_enabled");
        c.draw = ctx->param_bool("draw_status");
        c.work_roi = ctx->param_string("work_roi");
        c.block_prefix = ctx->param_string("occluder_prefix");
        c.clear_roi[OUTFIT] = ctx->param_string("outfit_clear_roi");
        c.clear_roi[HELMET] = ctx->param_string("helmet_clear_roi");
        c.clear_roi[GLOVE] = ctx->param_string("glove_clear_roi");
        return c;
    }
    std::string signature(const Config &c)
    {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << std::setprecision(9) << c.dedupe_iou << '|' << c.score << '|' << c.low_score << '|' << c.overlap << '|' << c.match_distance << '|' << c.missing_ratio
          << '|' << c.head_scale << '|' << c.body_width_scale << '|' << c.body_height_scale << '|' << c.model_index
          << '|' << c.min_samples << '|' << c.required_gloves << '|' << c.min_part_pixels << '|' << c.max_sessions
          << '|' << c.sample_ms << '|' << c.window_ms << '|' << c.span_ms << '|' << c.warmup_ms << '|' << c.retention_ms
          << '|' << c.gap_ms << '|' << c.enabled << '|' << c.gloves_enabled << '|' << c.work_roi << '|' << c.block_prefix;
        for (const auto &name : c.clear_roi)
            o << '|' << name;
        return o.str();
    }
    cv::Point2f center(const Box &b) { return {b.x + b.width / 2, b.y + b.height / 2}; }
    float area(const Box &b) { return std::max(0.f, b.width) * std::max(0.f, b.height); }
    float overlap(const Box &a, const Box &b)
    {
        return area(a & b) / std::max(1.f, std::min(area(a), area(b)));
    }
    Box expand(const Box &b, float x, float y)
    {
        return {b.x - b.width * x, b.y - b.height * y, b.width * (1 + 2 * x), b.height * (1 + 2 * y)};
    }
    Box estimated_head(const Box &outfit, const Config &c)
    {
        float size = outfit.width * c.head_scale;
        return {center(outfit).x - size / 2, outfit.y - size, size, size};
    }
    Box estimated_body(const Box &helmet, const Config &c)
    {
        float width = helmet.width * c.body_width_scale;
        return {center(helmet).x - width / 2, helmet.y + helmet.height, width, width * c.body_height_scale};
    }
    Box hands_area(const Candidate &a)
    {
        return {a.body.x - a.body.width * .45f, a.body.y + a.body.height * .1f,
                a.body.width * 1.9f, a.body.height * .9f};
    }
    const RoiZone *valid_roi(ChannelContext *ctx, const std::string &name)
    {
        const auto *r = name.empty() ? nullptr : ctx->roi_by_name(name.c_str());
        return r && r->polygon.size() >= 3 ? r : nullptr;
    }
    bool inside(const RoiZone *r, cv::Point2f p)
    {
        return r && cv::pointPolygonTest(r->polygon, p, false) >= 0;
    }
    // Exact conservative polygon/rectangle contact, including thin occluders.
    double cross(cv::Point2f a, cv::Point2f b, cv::Point2f c)
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }
    bool segments(cv::Point2f a, cv::Point2f b, cv::Point2f c, cv::Point2f d)
    {
        if (std::max(a.x, b.x) < std::min(c.x, d.x) || std::max(c.x, d.x) < std::min(a.x, b.x) ||
            std::max(a.y, b.y) < std::min(c.y, d.y) || std::max(c.y, d.y) < std::min(a.y, b.y))
            return false;
        return cross(a, b, c) * cross(a, b, d) <= 0 && cross(c, d, a) * cross(c, d, b) <= 0;
    }
    std::array<cv::Point2f, 4> corners(const Box &b)
    {
        return {{{b.x, b.y}, {b.x + b.width, b.y}, {b.x + b.width, b.y + b.height}, {b.x, b.y + b.height}}};
    }
    bool boundary_touches(const RoiZone *r, const Box &b)
    {
        auto q = corners(b);
        for (size_t i = 0; i < r->polygon.size(); ++i)
            for (size_t j = 0; j < 4; ++j)
                if (segments(r->polygon[i], r->polygon[(i + 1) % r->polygon.size()], q[j], q[(j + 1) % 4]))
                    return true;
        return false;
    }
    bool touches(const RoiZone *r, const Box &b)
    {
        for (const auto &p : corners(b))
            if (inside(r, p))
                return true;
        for (const auto &p : r->polygon)
            if (b.contains(cv::Point2f(p)))
                return true;
        return boundary_touches(r, b);
    }
    bool contained_strictly(const RoiZone &zone, const Box &b)
    {
        if (zone.polygon.size() < 3)
            return false;
        for (const auto &p : corners(b))
            if (!inside(&zone, p))
                return false;
        return !boundary_touches(&zone, b);
    }
    bool inference_covers(ChannelContext *ctx, const Box &b)
    {
        if (!ctx->config)
            return false;
        const auto &r = ctx->config->inference_roi;
        if (!r.restricts_results_to_roi())
            return true;
        RoiZone zone;
        const double w = ctx->business_width(), h = ctx->business_height();
        if (r.has_polygon())
        {
            for (const auto &p : r.polygon)
                zone.polygon.push_back(cv::Point(static_cast<int>(std::lround(p.first * w)),
                                                 static_cast<int>(std::lround(p.second * h))));
        }
        else
        {
            const int x = static_cast<int>(std::lround(r.x * w)), y = static_cast<int>(std::lround(r.y * h));
            const int right = static_cast<int>(std::lround((r.x + r.width) * w));
            const int bottom = static_cast<int>(std::lround((r.y + r.height) * h));
            if (right <= x || bottom <= y)
                return false;
            zone.polygon = {{x, y}, {right, y}, {right, bottom}, {x, bottom}};
        }
        return contained_strictly(zone, b);
    }
    const ChannelModelConfig *selected_model(ChannelContext *ctx, const Config &c)
    {
        if (!ctx->config)
            return nullptr;
        int index = 0;
        for (const auto &m : ctx->config->models)
        {
            if (!m.enable || m.model_path.empty() || m.model_type.empty())
                continue;
            if (index++ == c.model_index)
                return &m;
        }
        return nullptr;
    }
    // 返回阻止本部位判断的原因；空字符串只表示通过几何检查。
    std::string inspection_block(ChannelContext *ctx, const Config &c, int item, const Box &b)
    {
        if (b.width < c.min_part_pixels || b.height < c.min_part_pixels)
            return "too_small";
        if (b.x < 0 || b.y < 0 || b.x + b.width > ctx->business_width() ||
            b.y + b.height > ctx->business_height())
            return "frame_edge";
        if (!inference_covers(ctx, b))
            return "outside_inference_coverage";
        const auto *r = valid_roi(ctx, c.clear_roi[item]);
        if (!r)
            return "clear_roi_missing";
        for (const auto &p : corners(b))
            if (!inside(r, p))
                return "outside_clear_roi";
        if (boundary_touches(r, b))
            return "clear_roi_boundary";
        for (int i = 0; i < ctx->roi_count(); ++i)
        {
            const auto *block = ctx->roi_at(i);
            if (block && block->polygon.size() >= 3 && !c.block_prefix.empty() &&
                block->name.compare(0, c.block_prefix.size(), c.block_prefix) == 0 && touches(block, b))
                return "static_occlusion";
        }
        return "";
    }

    std::vector<Candidate> candidates(const std::vector<Detection> &d, const Config &c)
    {
        std::vector<Candidate> out;
        std::vector<std::vector<int>> links(d.size());
        for (size_t i = 0; i < d.size(); ++i)
            if (d[i].cls == OUTFIT)
                for (size_t j = 0; j < d.size(); ++j)
                    if (d[j].cls == HELMET &&
                        expand(estimated_head(d[i].box, c), .65f, .65f).contains(center(d[j].box)))
                    {
                        links[i].push_back(static_cast<int>(j));
                        links[j].push_back(static_cast<int>(i));
                    }
        std::vector<bool> used(d.size(), false);
        for (size_t i = 0; i < d.size(); ++i)
            if (d[i].cls == OUTFIT)
            {
                Candidate a;
                a.body = d[i].box;
                a.head = estimated_head(a.body, c);
                a.source[OUTFIT] = static_cast<int>(i);
                a.tracker[OUTFIT] = d[i].tracker;
                if (links[i].size() == 1 && links[links[i][0]].size() == 1)
                {
                    int j = links[i][0];
                    used[j] = true;
                    a.head = d[j].box;
                    a.source[HELMET] = j;
                    a.tracker[HELMET] = d[j].tracker;
                }
                else if (!links[i].empty())
                    a.ambiguous = true;
                out.push_back(a);
            }
        for (size_t j = 0; j < d.size(); ++j)
            if (d[j].cls == HELMET && !used[j])
            {
                Candidate a;
                a.head = d[j].box;
                a.body = estimated_body(a.head, c);
                a.source[HELMET] = static_cast<int>(j);
                a.tracker[HELMET] = d[j].tracker;
                a.ambiguous = !links[j].empty();
                out.push_back(a);
            }
        for (size_t i = 0; i < out.size(); ++i)
            for (size_t j = i + 1; j < out.size(); ++j)
                if (overlap(out[i].body, out[j].body) >= c.overlap || overlap(out[i].head, out[j].head) > .2f)
                    out[i].ambiguous = out[j].ambiguous = true;
        return out;
    }
    bool near(const Candidate &a, const Candidate &b, const Config &c)
    {
        auto p = center(a.head), q = center(b.head);
        float size = std::max(1.f, std::max(a.head.width, b.head.width));
        float ratio = a.head.width / std::max(1.f, b.head.width);
        return ratio > .4f && ratio < 2.5f && std::hypot(p.x - q.x, p.y - q.y) / size < c.match_distance;
    }
    void clear_evidence(Session &s)
    {
        for (auto &e : s.evidence)
            e.clear();
        s.status = {{-1, -1, -1}};
        s.reason = {{"unobserved", "unobserved", "unobserved"}};
        s.current_mask = 0;
        s.sampled = false;
    }
    std::string names(unsigned mask)
    {
        std::string r;
        const char *labels[] = {"outfit", "helmet", "glove"};
        for (int k = 0; k < 3; ++k)
            if (mask & (1u << k))
            {
                if (!r.empty())
                    r += ",";
                r += labels[k];
            }
        return r;
    }
    bool confirmed(const std::deque<Sample> &e, const Config &c)
    {
        if (e.size() < static_cast<size_t>(c.min_samples))
            return false;
        size_t count = 0;
        uint64_t first = 0, last = 0;
        for (const auto &v : e)
            if (v.missing)
            {
                if (count++ == 0)
                    first = v.time;
                last = v.time;
            }
        return count >= static_cast<size_t>(c.min_samples) && last - first >= c.span_ms &&
               static_cast<double>(count) / e.size() >= c.missing_ratio && e.back().missing;
    }

    // 不依赖 -ffast-math 下可能被优化掉的 std::isfinite。
    bool valid_score(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7f800000U) != 0x7f800000U && value >= 0.f && value <= 1.f;
    }
    float iou(const Box &a, const Box &b)
    {
        const float intersection = area(a & b);
        return intersection / std::max(1.f, area(a) + area(b) - intersection);
    }
    std::vector<Detection> deduplicate(std::vector<Detection> input, float threshold)
    {
        std::stable_sort(input.begin(), input.end(), [](const Detection &a, const Detection &b)
                         { return a.score > b.score; });
        std::vector<Detection> kept;
        for (const auto &d : input)
        {
            bool duplicate = false;
            for (const auto &k : kept)
                if (k.cls == d.cls && iou(k.box, d.box) >= threshold)
                {
                    duplicate = true;
                    break;
                }
            if (!duplicate)
                kept.push_back(d);
        }
        return kept;
    }
    bool same_equipment_track(const Candidate &a, const Candidate &b)
    {
        // 装备的跟踪 ID 仅是辅助线索；调用者还必须检查空间位置及一对一关系。
        for (int k = 0; k < 2; ++k)
            if (a.tracker[k] >= 0 && b.tracker[k] >= 0 && a.tracker[k] == b.tracker[k])
                return true;
        return false;
    }
    void clear_all(State &s)
    {
        for (auto &person : s.sessions)
            clear_evidence(person);
    }
    void idle(ChannelContext *ctx, const Config &c, const std::string &status)
    {
        ctx->publish_int("candidate_count", 0);
        ctx->publish_int("suspected_count", 0);
        ctx->publish_int("unresolved_count", 0);
        ctx->publish_bool("ppe_alarm", false);
        ctx->publish_json("sessions", "[]");
        ctx->publish_json("equipment_counts", "{\"outfit\":0,\"helmet\":0,\"glove\":0}");
        ctx->publish_string("status", status);
        if (c.draw)
            draw_text(ctx, ("PPE V2: " + status).c_str(), {12, 24}, {0, 200, 255}, .45, 1);
    }
    std::string session_key(ChannelContext *ctx, const State &s, const Session &person)
    {
        return std::to_string(ctx->chnId) + "-" + std::to_string(s.epoch) + "-" + std::to_string(person.id);
    }

    // 一对一分配，ID 辅助优先；任何身份歧义都不会合并两人的报警锁。
    std::vector<int> associate(std::vector<Candidate> &people, State &state, const Config &c,
                               const RoiZone *work, uint64_t now, bool &capacity)
    {
        const size_t old_count = state.sessions.size();
        std::vector<std::vector<int>> choices(people.size());
        std::vector<int> owners(old_count, 0), assigned(people.size(), -1);
        for (size_t i = 0; i < people.size(); ++i)
        {
            std::vector<int> nearby, by_id;
            for (size_t j = 0; j < old_count; ++j)
                if (near(people[i], state.sessions[j].anchor, c))
                {
                    nearby.push_back(static_cast<int>(j));
                    if (same_equipment_track(people[i], state.sessions[j].anchor))
                        by_id.push_back(static_cast<int>(j));
                }
            choices[i] = by_id.empty() ? nearby : by_id;
            for (int j : choices[i])
                ++owners[j];
        }
        for (size_t i = 0; i < people.size(); ++i)
        {
            if (choices[i].size() == 1 && owners[choices[i][0]] == 1)
                assigned[i] = choices[i][0];
            else if (!choices[i].empty())
            {
                people[i].ambiguous = true;
                for (int j : choices[i])
                {
                    clear_evidence(state.sessions[j]);
                    state.sessions[j].touched = now;
                }
            }
            else if (!people[i].ambiguous && inside(work, center(people[i].body)))
            {
                if (state.sessions.size() >= static_cast<size_t>(c.max_sessions))
                {
                    capacity = true;
                    continue;
                }
                Session person;
                person.id = state.next_id++;
                person.first_seen = now;
                person.last_seen = now;
                person.touched = now;
                person.anchor = people[i];
                assigned[i] = static_cast<int>(state.sessions.size());
                state.sessions.push_back(person);
            }
        }
        return assigned;
    }

    // 只在当前帧确有有效缺失观察且时间窗证据达到门槛时返回非零 mask。
    void observe(ChannelContext *ctx, Session &person, const Candidate &p, const Config &c,
                 const std::vector<Detection> &weak, int glove_count, bool glove_ambiguous,
                 bool in_work, uint64_t now)
    {
        person.current_mask = 0;
        person.glove_count = glove_count;
        bool crowded = p.ambiguous;
        for (const auto &d : weak)
            if (d.cls != GLOVE)
            {
                const Box b = d.cls == OUTFIT ? d.box : estimated_body(d.box, c);
                if (overlap(b, p.body) > .05f)
                    crowded = true;
            }
        if (crowded || !in_work)
        {
            clear_evidence(person);
            const std::string reason = crowded ? "crowd_or_ownership" : "outside_work_roi";
            person.reason = {{reason, reason, reason}};
            return;
        }
        const std::array<Box, 3> parts{{p.body, expand(p.head, .15f, .15f), hands_area(p)}};
        const std::array<bool, 3> present{{p.source[OUTFIT] >= 0, p.source[HELMET] >= 0, glove_count >= c.required_gloves}};
        const bool take_sample = !person.sampled || now - person.last_sample >= c.sample_ms;
        if (take_sample)
        {
            person.sampled = true;
            person.last_sample = now;
        }
        for (int k = 0; k < 3; ++k)
        {
            auto &e = person.evidence[k];
            while (!e.empty() && now - e.front().time > c.window_ms)
                e.pop_front();
            std::string reason = inspection_block(ctx, c, k, parts[k]);
            if (k == GLOVE && !c.gloves_enabled)
                reason = "disabled";
            if (k == GLOVE && glove_ambiguous)
                reason = "glove_ownership";
            for (const auto &d : weak)
                if (d.cls == k && overlap(d.box, parts[k]) > 0)
                    reason = "low_confidence";
            person.reason[k] = reason.empty() ? (present[k] ? "equipment_detected" : "equipment_not_detected") : reason;
            person.status[k] = reason.empty() ? (present[k] ? 0 : 1) : -1;
            // 可见性与归属每帧更新，不让上一采样时刻的缺失状态跨遮挡帧触发事件。
            if (take_sample && reason.empty())
                e.push_back(Sample{now, !present[k]});
            if (person.status[k] == 1 && now - person.first_seen >= c.warmup_ms && confirmed(e, c))
                person.current_mask |= 1u << k;
        }
        person.confirmed_mask |= person.current_mask;
    }

    void draw_person(ChannelContext *ctx, const Session &person, const Config &c)
    {
        if (!c.draw)
            return;
        const Box &b = person.anchor.body;
        cv::Rect box(static_cast<int>(b.x), static_cast<int>(b.y), static_cast<int>(b.width), static_cast<int>(b.height));
        box &= cv::Rect(0, 0, ctx->business_width(), ctx->business_height());
        if (box.empty())
            return;
        const cv::Scalar color = person.current_mask ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 200, 255);
        draw_rect(ctx, box, color, 2);
        const auto mark = [](int v)
        { return v < 0 ? '?' : v == 0 ? 'P'
                                      : 'M'; };
        std::ostringstream label;
        label << "PPE#" << person.id << " O:" << mark(person.status[0]) << " H:" << mark(person.status[1])
              << " G:" << mark(person.status[2]);
        if (person.current_mask)
            label << " SUSPECT";
        if (person.notified)
            label << " [reported]";
        draw_text(ctx, label.str().c_str(), {box.x, std::min(ctx->business_height() - 8, box.y + box.height + 15)}, color, .4, 1);
    }

    void submit(ChannelContext *ctx, State &state, Session &person, const Config &c, uint64_t now)
    {
        if (!person.current_mask || !c.enabled || person.notified ||
            (person.attempted && now - person.last_attempt < 1000))
            return;
        person.attempted = true;
        person.last_attempt = now;
        EventRequest request;
        request.event_type = "ppe_yolov8s_v2_suspected";
        request.message = "防护装备疑似缺失（需复核）: " + names(person.current_mask);
        request.merge_mode = EventMergeMode::NEVER; // 不允许框架将不同人员按时间窗口合并。
        request.fields = {event_field("person_session_id", session_key(ctx, state, person)),
                          event_field("missing_items", names(person.current_mask)), event_field("suspected", true),
                          event_field("glove_count", person.glove_count), event_field("work_roi", c.work_roi),
                          event_field("basis", "positive_detection_absence_with_geometry_gates"),
                          event_field("evidence_window_ms", c.window_ms)};
        const EventReportResult report = report_event(ctx, request);
        if (report.accepted())
        {
            person.notified = true;
            person.event_id = report.event_id;
        }
        else if (!state.error_logged || now - state.last_error >= 30000)
        {
            state.error_logged = true;
            state.last_error = now;
            std::fprintf(stderr, "[logic_ppe_yolov8s_v2][ch%d] event rejected: %s (%s)\n", ctx->chnId,
                         event_report_status_name(report.status), report.detail.c_str());
        }
    }

    static void logic_ppe_yolov8s_v2(ChannelContext *ctx)
    {
        if (!ctx)
            return;
        State *ptr = ctx->get_state<State>();
        if (!ptr)
            return;
        State &state = *ptr;
        const Config c = config(ctx);
        const uint64_t now = ctx->timestamp_ms;
        if (!state.clock_started)
            state.epoch = ctx->unix_ms ? ctx->unix_ms : now;
        const bool clock_back = state.clock_started && now < state.last_time;
        const bool restart = state.clock_started && ctx->frame_id < state.last_frame;
        const bool gap = state.clock_started && (clock_back || restart || now - state.last_time > c.gap_ms);
        if (gap)
        {
            clear_all(state);
            state.last_result_frame = -1;
            for (auto &s : state.sessions)
            {
                s.first_seen = now;
                s.attempted = false;
                if (clock_back)
                {
                    s.last_seen = now;
                    s.touched = now;
                }
            }
            state.error_logged = false;
        }
        const bool duplicate = state.clock_started && !clock_back && !restart && ctx->frame_id == state.last_frame;
        state.clock_started = true;
        state.last_time = now;
        state.last_frame = ctx->frame_id;
        std::ostringstream version;
        version.imbue(std::locale::classic());
        version << signature(c);
        const ChannelModelConfig *model = selected_model(ctx, c);
        if (ctx->config)
        {
            const auto &r = ctx->config->inference_roi;
            version << '|' << r.mode << '|' << r.resize_mode << '|' << r.x << ',' << r.y << ',' << r.width << ',' << r.height;
            for (const auto &p : r.polygon)
                version << ':' << p.first << ',' << p.second;
        }
        if (model)
        {
            version << '|' << model->id << '|' << model->model_path << '|' << model->label_path << '|' << model->version
                    << '|' << model->obj_thresh << '|' << model->nms_thresh;
            for (const auto &name : model->detect_classes)
                version << '|' << name;
        }
        for (int i = 0; i < ctx->roi_count(); ++i)
        {
            const auto *roi = ctx->roi_at(i);
            if (roi)
            {
                version << '|' << roi->name;
                for (const auto &p : roi->polygon)
                    version << ':' << p.x << ',' << p.y;
            }
        }
        if (version.str() != state.config_signature)
        {
            clear_all(state);
            for (auto &s : state.sessions)
                s.first_seen = now;
            state.config_signature = version.str();
        }
        state.sessions.erase(std::remove_if(state.sessions.begin(), state.sessions.end(),
                                            [&](const Session &s)
                                            { return now - s.touched > c.retention_ms; }),
                             state.sessions.end());
        if (c.sample_ms == 0 || c.span_ms > c.window_ms || c.min_samples < 2 ||
            static_cast<uint64_t>(c.min_samples) > c.window_ms / c.sample_ms + 1)
        {
            clear_all(state);
            idle(ctx, c, "invalid_evidence_parameters");
            return;
        }
        if (!ctx->infer_enabled || !ctx->inference_valid || !ctx->results)
        {
            clear_all(state);
            idle(ctx, c, "inference_unavailable");
            return;
        }
        if (!model)
        {
            clear_all(state);
            idle(ctx, c, "model_not_configured");
            return;
        }
        if (!model->detect_classes.empty())
        {
            for (const std::string name : {"outfit", "helmet", "glove"})
                if (std::find(model->detect_classes.begin(), model->detect_classes.end(), name) == model->detect_classes.end())
                {
                    clear_all(state);
                    idle(ctx, c, "required_class_filtered");
                    return;
                }
        }
        const auto *work = valid_roi(ctx, c.work_roi);
        if (!work)
        {
            clear_all(state);
            idle(ctx, c, "work_roi_missing");
            return;
        }
        if (duplicate)
        {
            idle(ctx, c, "duplicate_frame_ignored");
            return;
        }

        std::vector<Detection> raw;
        const char *labels[] = {"outfit", "helmet", "glove"};
        int64_t batch = -1;
        for (const auto &r : *ctx->results)
        {
            if (r.model_index != c.model_index)
                continue;
            if (r.is_depth() || r.is_pose())
            {
                clear_all(state);
                idle(ctx, c, "wrong_model_kind");
                return;
            }
            if ((r.frame_id > 0 && r.frame_id != ctx->frame_id) ||
                (r.timestamp_ms > 0 && (r.timestamp_ms > now || now - r.timestamp_ms > c.gap_ms)))
            {
                clear_all(state);
                idle(ctx, c, "stale_or_mixed_results");
                return;
            }
            if (r.frame_id > 0)
                batch = std::max(batch, r.frame_id);
            if (!valid_score(r.score) || r.score < c.low_score)
                continue;
            if (r.class_id < 0 || r.class_id > 2 || (!r.label.empty() && r.label != labels[r.class_id]))
            {
                clear_all(state);
                idle(ctx, c, "class_mapping_mismatch");
                return;
            }
            const Box b(r.box);
            if (b.width <= 0 || b.height <= 0 || b.x < 0 || b.y < 0 ||
                b.x + b.width > ctx->business_width() || b.y + b.height > ctx->business_height())
            {
                clear_all(state);
                idle(ctx, c, "invalid_detection_box");
                return;
            }
            raw.push_back(Detection{b, r.class_id, r.track_id, r.score});
            if (raw.size() > 256)
            {
                clear_all(state);
                idle(ctx, c, "too_many_detections");
                return;
            }
        }
        if (batch >= 0 && batch <= state.last_result_frame)
        {
            idle(ctx, c, "reused_results_ignored");
            return;
        }
        if (batch >= 0)
            state.last_result_frame = batch;
        const auto filtered = deduplicate(raw, c.dedupe_iou);
        std::vector<Detection> detections, weak;
        std::array<int, 3> counts{{0, 0, 0}};
        for (const auto &d : filtered)
        {
            if (d.score >= c.score)
            {
                detections.push_back(d);
                ++counts[d.cls];
            }
            else
                weak.push_back(d);
        }
        auto people = candidates(detections, c);
        bool capacity = false;
        const auto assigned = associate(people, state, c, work, now, capacity);
        std::vector<bool> observed(state.sessions.size(), false), glove_ambiguous(people.size(), false);
        std::vector<int> gloves(people.size(), 0);
        for (const auto &d : detections)
            if (d.cls == GLOVE)
            {
                std::vector<size_t> owners;
                for (size_t i = 0; i < people.size(); ++i)
                    if (hands_area(people[i]).contains(center(d.box)))
                        owners.push_back(i);
                if (owners.size() == 1)
                    ++gloves[owners[0]];
                else
                    for (size_t i : owners)
                        glove_ambiguous[i] = true;
            }
        int active = 0, suspected = 0, unresolved = 0;
        for (size_t i = 0; i < people.size(); ++i)
        {
            const auto &p = people[i];
            if (assigned[i] < 0)
            {
                if (inside(work, center(p.body)))
                {
                    ++unresolved;
                    if (c.draw)
                        draw_text(ctx, "PPE identity unresolved", {std::max(0, static_cast<int>(p.body.x)), std::max(18, static_cast<int>(p.body.y))}, {0, 200, 255}, .4, 1);
                }
                continue;
            }
            const size_t j = static_cast<size_t>(assigned[i]);
            Session &s = state.sessions[j];
            observed[j] = true;
            s.touched = now;
            if (now - s.last_seen > c.gap_ms)
            {
                clear_evidence(s);
                s.first_seen = now;
            }
            if (!p.ambiguous)
            {
                // 保留暂时消失的装备 ID 作为下一帧的辅助线索；从不跨类别比较 ID。
                const auto old_ids = s.anchor.tracker;
                s.anchor = p;
                for (int k = 0; k < 2; ++k)
                    if (s.anchor.tracker[k] < 0)
                        s.anchor.tracker[k] = old_ids[k];
                s.last_seen = now;
            }
            const bool in_work = inside(work, center(p.body));
            observe(ctx, s, p, c, weak, gloves[i], glove_ambiguous[i], in_work, now);
            if (in_work)
            {
                ++active;
                if (s.current_mask)
                    ++suspected;
            }
            draw_person(ctx, s, c);
        }
        for (size_t j = 0; j < state.sessions.size(); ++j)
            if (!observed[j])
                clear_evidence(state.sessions[j]);
        bool missing_clear_roi = false;
        for (int k = 0; k < 3; ++k)
            if ((k != GLOVE || c.gloves_enabled) && !valid_roi(ctx, c.clear_roi[k]))
                missing_clear_roi = true;
        const std::string status = capacity ? "session_capacity" : missing_clear_roi ? "inspection_roi_missing"
                                                               : !c.enabled          ? "alarms_disabled"
                                                               : people.empty()      ? "no_person_anchor"
                                                               : unresolved          ? "identity_ambiguous"
                                                                                     : "suspected_only";
        if (c.draw)
        {
            std::ostringstream text;
            text << "PPE V2 " << status << " candidates=" << active << " suspect=" << suspected;
            draw_text(ctx, text.str().c_str(), {12, 24}, {0, 200, 255}, .4, 1);
        }
        // 所有绘制完成后再上报，确保本帧证据含完整标注。不上报未确认的历史 mask。
        for (size_t j = 0; j < state.sessions.size(); ++j)
            if (observed[j])
                submit(ctx, state, state.sessions[j], c, now);
        std::ostringstream payload;
        payload.imbue(std::locale::classic());
        payload << '[';
        bool comma = false;
        for (size_t j = 0; j < state.sessions.size(); ++j)
            if (observed[j])
            {
                const auto &s = state.sessions[j];
                if (comma)
                    payload << ',';
                comma = true;
                // 所有字符串字段都来自数字或固定原因码，不插入用户可控的未转义 JSON 文本。
                payload << "{\"id\":\"" << session_key(ctx, state, s) << "\",\"outfit\":" << s.status[0]
                        << ",\"helmet\":" << s.status[1] << ",\"glove\":" << s.status[2]
                        << ",\"reasons\":[\"" << s.reason[0] << "\",\"" << s.reason[1] << "\",\"" << s.reason[2]
                        << "\"],\"suspected_mask\":" << s.current_mask << ",\"history_mask\":" << s.confirmed_mask
                        << ",\"glove_count\":" << s.glove_count << ",\"reported\":" << (s.notified ? "true" : "false") << '}';
            }
        payload << ']';
        std::ostringstream equipment;
        equipment << "{\"outfit\":" << counts[0] << ",\"helmet\":" << counts[1] << ",\"glove\":" << counts[2] << '}';
        ctx->publish_int("candidate_count", active);
        ctx->publish_int("suspected_count", suspected);
        ctx->publish_int("unresolved_count", unresolved);
        ctx->publish_bool("ppe_alarm", suspected > 0);
        ctx->publish_string("status", status);
        ctx->publish_json("sessions", payload.str());
        ctx->publish_json("equipment_counts", equipment.str());
    }
} // namespace

REGISTER_LOGIC(logic_ppe_yolov8s_v2);
