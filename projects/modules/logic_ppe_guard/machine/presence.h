#pragma once
#include "../ppe_guard.h"
// 850T platform occupancy. Geometry and all track decisions use the SDK business canvas.
#include <algorithm>
#include <channel.h>
#include <drawing.h>
#include <events.h>
#include <map>
#include <opencv2/imgproc.hpp>
#include <set>
#include <string>
#include <vector>

namespace machine_presence
{
using Point = cv::Point;
using Polygon = std::vector<Point>;

struct Visit
{
    int id = 0; // Stable visit number; a tracker ID can change after occlusion.
    int track = -1;
    Point last;
    uint64_t seen = 0;
    bool approaching = false;
    bool hidden = false;
    bool reported = false;
    std::string evidence;
    uint64_t last_near_door = 0;
    bool missing_at_door = false;
    uint64_t next_evidence = 0;
    uint64_t next_alarm = 0;
};

struct Step
{
    Point point;
    bool inside;
    uint64_t time;
};

struct State
{
    std::map<int, Visit> visits;
    std::map<int, Step> steps;
    int next_id = 1;
    uint64_t previous = 0;
    uint64_t initial_time = 0;
    bool started = false;
    std::map<int, uint64_t> initial_candidates;
    Polygon platform, blind;
};

const cv::Scalar green(110, 210, 100), cyan(230, 210, 60), amber(30, 180, 250), red(40, 50, 240);

inline void caption(ChannelContext *ctx, const std::string &s, Point p, const cv::Scalar &color)
{
    draw_text(ctx, s.c_str(), p, color, 0.53, 1, DrawCommand::ALL, true);
}

inline bool crosses_gate(Point a, Point b, Point g, Point h)
{
    const auto cross = [](Point a, Point b, Point p) { return (b - a).cross(p - a); };
    return cross(g, h, a) * cross(g, h, b) < 0 && cross(a, b, g) * cross(a, b, h) <= 0;
}

inline Visit *tracked(State &s, int track)
{
    for (auto &entry : s.visits)
        if (entry.second.track == track)
            return &entry.second;
    return nullptr;
}

inline void clear_episode(Visit &v)
{
    v.hidden = v.approaching = v.reported = false;
    v.missing_at_door = false;
    v.evidence.clear();
    v.next_alarm = v.next_evidence = 0;
}

// Keep fixed tracker matches. Among nearby replacement tracks, use as many distinct
// visits as the observations support. A single detection can recover only one visit.
using ReturnCandidates = std::map<int, std::vector<std::pair<double, int>>>;
inline bool assign_return(int track, const ReturnCandidates &candidates, std::map<int, int> &owners,
                          std::set<int> &searched)
{
    const auto &nearby = candidates.at(track);
    for (const auto &candidate : nearby)
        if (!owners.count(candidate.second))
        {
            owners.emplace(candidate.second, track);
            return true;
        }
    for (const auto &candidate : nearby)
        if (searched.insert(candidate.second).second &&
            assign_return(owners.at(candidate.second), candidates, owners, searched))
        {
            owners.at(candidate.second) = track;
            return true;
        }
    return false;
}

inline void update(ChannelContext *ctx, State &s)
{
    const auto *platform = ctx->roi_polygon_at(roi_find(ctx, ctx->param_string("platform_roi").c_str()));
    const auto *blind = ctx->roi_polygon_at(roi_find(ctx, ctx->param_string("blind_roi").c_str()));
    const bool geometry = platform && blind && platform != blind && platform->size() >= 3 && blind->size() >= 3 &&
                          cv::contourArea(*platform) > 1 && cv::contourArea(*blind) > 1 &&
                          (*platform)[0] != (*platform)[1];
    const uint64_t now = ctx->timestamp_ms;
    const uint64_t confirm = ctx->param_int("missing_ms");
    const uint64_t timeout = 1000ULL * ctx->param_int("timeout_sec");
    const uint64_t continuity = 1500;
    if (now < s.previous || (geometry && (s.platform != *platform || s.blind != *blind)))
    {
        s = State{};
        if (geometry)
        {
            s.platform = *platform;
            s.blind = *blind;
        }
    }
    const bool valid = geometry && ctx->infer_enabled && ctx->inference_valid && ctx->results;
    if (!valid || now - s.previous > continuity)
    {
        s.steps.clear();
        for (auto &item : s.visits)
            if (!item.second.hidden)
                clear_episode(item.second);
    }
    if (valid && !s.started)
    {
        s.started = true;
        s.initial_time = now;
    }
    const bool initial_window = valid && ctx->param_bool("initialize_visible") && now - s.initial_time <= 4000;
    s.previous = now;

    if (geometry)
    {
        draw_polyline(ctx, *platform, green, 2, 1.0, true);
        draw_polyline(ctx, *blind, amber, 2, 1.0, true);
        draw_line(ctx, (*platform)[0], (*platform)[1], cyan, 4);
    }

    const auto labels = ppe_guard::split_labels(ctx->param_string("person_labels"));
    const float score = ctx->param_float("person_min_score");
    const int gate_height = ctx->param_int("gate_standing_height");
    const int machine_height = ctx->param_int("machine_standing_height");
    const int min_observed_height = ctx->param_int("min_platform_person_height");
    const double gate_x = geometry ? (platform->at(0).x + platform->at(1).x) * 0.5 : 0;
    const double machine_x = geometry ? cv::minAreaRect(*blind).center.x : 0;
    int visible = 0;
    std::map<int, Point> observed;
    bool untracked = false;
    int next_anonymous = -1;
    if (valid)
        for (const auto &r : *ctx->results)
        {
            if (!ppe_guard::matches_label(r.label, labels) || r.score < score || r.box.empty())
                continue;
            // Keep following an enrolled worker during partial occlusion; a new small fragment
            // cannot establish an additional person on the platform.
            const int height_floor = tracked(s, r.track_id) ? min_observed_height * 2 / 3 : min_observed_height;
            if (r.box.height < height_floor)
                continue;
            Point p(r.box.x + r.box.width / 2, r.box.y + r.box.height - 1);
            // A torso-only detector box ends at the railing. Estimate the floor anchor using
            // the two measured standing heights for this camera; keep a genuinely lower observed foot.
            if (gate_height > 0 && machine_height > 0)
            {
                const double fraction = std::abs(machine_x - gate_x) > 1
                                            ? std::max(0.0, std::min(1.0, (p.x - gate_x) / (machine_x - gate_x)))
                                            : 0.5;
                const int standing_height = cvRound(gate_height + fraction * (machine_height - gate_height));
                p.y = std::max(p.y, r.box.y + standing_height - 1);
            }
            const int observation = r.track_id >= 0 ? r.track_id : next_anonymous--;
            if (!observed.emplace(observation, p).second)
                continue;
            if (cv::pointPolygonTest(*platform, p, false) >= 0)
                ++visible;
            if (r.track_id < 0 && cv::pointPolygonTest(*platform, p, false) >= 0)
                untracked = true;
        }

    ReturnCandidates candidates;
    for (const auto &o : observed)
    {
        if (tracked(s, o.first) || cv::pointPolygonTest(*platform, o.second, false) < 0)
            continue;
        const auto last = s.steps.find(o.first);
        if (last != s.steps.end() && !last->second.inside && now - last->second.time <= continuity &&
            crosses_gate(last->second.point, o.second, platform->at(0), platform->at(1)))
            continue;
        for (const auto &entry : s.visits)
        {
            const Visit &v = entry.second;
            if (observed.count(v.track) || (!v.hidden && now - v.seen > continuity))
                continue;
            // A worker may first become visible on the platform side of the door outline.
            const double distance = cv::norm(o.second - v.last);
            if (distance <= 55)
                candidates[o.first].emplace_back(distance, v.id);
        }
    }
    std::vector<int> order;
    for (auto &c : candidates)
    {
        std::sort(c.second.begin(), c.second.end());
        order.push_back(c.first);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if ((a < 0) != (b < 0))
            return a >= 0;
        return candidates.at(a).front().first < candidates.at(b).front().first;
    });
    std::map<int, int> owners; // visit -> observation
    for (int observation : order)
    {
        std::set<int> searched;
        assign_return(observation, candidates, owners, searched);
    }
    std::map<int, int> anonymous_visits;
    for (const auto &match : owners)
    {
        Visit &v = s.visits.at(match.first);
        if (match.second < 0)
            anonymous_visits.emplace(match.second, v.id);
        else
        {
            // The first box with the replacement ID may already be across the door.
            // Carry its visit's last step so that frame can register the crossing.
            const auto previous = s.steps.find(v.track);
            if (!v.hidden && previous != s.steps.end() && now - previous->second.time <= continuity)
                s.steps[match.second] = previous->second;
            s.steps.erase(v.track);
            v.track = match.second;
        }
    }

    for (auto it = s.initial_candidates.begin(); it != s.initial_candidates.end();)
        if (!initial_window || !observed.count(it->first))
            it = s.initial_candidates.erase(it);
        else
            ++it;
    int visible_without_entry = 0;
    for (const auto &o : observed)
    {
        const int track = o.first;
        const Point p = o.second;
        const double distance = cv::pointPolygonTest(*platform, p, true);
        const bool inside = distance >= 0;
        const double door_distance = cv::pointPolygonTest(*blind, p, true);
        const bool at_blind = door_distance >= 0;
        Visit *v = tracked(s, track);
        if (track < 0)
        {
            const auto match = anonymous_visits.find(track);
            if (match == anonymous_visits.end())
                continue;
            v = &s.visits.at(match->second);
        }
        const int step_track = v ? v->track : track;
        auto prev = s.steps.find(step_track);
        const bool fresh = prev != s.steps.end() && now - prev->second.time <= continuity;
        const Point previous_point = fresh ? prev->second.point : p;
        const bool previous_inside = fresh && prev->second.inside;
        // A 3 px neutral band suppresses contact jitter. The finite first polygon edge is the staircase.
        if (std::abs(distance) >= 3)
        {
            bool crossing = fresh && prev->second.inside != inside && cv::norm(p - prev->second.point) <= 100 &&
                            crosses_gate(prev->second.point, p, platform->at(0), platform->at(1));
            bool initial_member = false;
            if (initial_window && inside && !v)
                initial_member = now - s.initial_candidates.emplace(track, now).first->second >= 1000;
            else
                s.initial_candidates.erase(track);
            if (inside && !v && (crossing || initial_member))
            {
                int id = s.next_id++;
                v = &s.visits[id];
                v->id = id;
                v->track = track;
            }
            if (crossing && !inside && v)
            {
                s.visits.erase(v->id);
                v = nullptr;
            }
            s.steps[step_track] = {p, inside, now};
        }
        else if (fresh)
            prev->second.time = now;
        if (!v)
        {
            if (inside)
                ++visible_without_entry;
            continue;
        }
        // A distant re-use of a hidden track ID cannot by itself clear an alarm.
        if (v->hidden && (!inside || (!at_blind && cv::norm(p - v->last) > 55)))
            continue;
        const bool on_return = v->hidden;
        // A jittering box at the doorway is one approach. Only a sustained, clear
        // departure rearms evidence collection; a disappearance far away never starts a timer.
        if (door_distance >= -10)
            v->last_near_door = now;
        if (on_return || (v->approaching && now - v->last_near_door >= 1000))
            clear_episode(*v);
        v->missing_at_door = door_distance >= -4;
        const bool enter_band = at_blind && !on_return && !v->approaching && fresh &&
                                cv::pointPolygonTest(*blind, previous_point, false) < 0 && previous_inside;
        if (enter_band)
            v->approaching = true;
        v->last = p;
        v->seen = now;
        if (at_blind && v->approaching && v->evidence.empty() && now >= v->next_evidence)
        {
            draw_circle(ctx, p, 6, amber, 2);
            EventRequest e;
            e.event_type = "machine_entry_evidence";
            e.message = "人员到达机台盲区边界";
            e.fields = {event_field("visit_id", v->id), event_field("track_id", v->track),
                        event_field("frame_id", ctx->frame_id)};
            auto result = report_event(ctx, e);
            if (result.accepted() && result.status != EventReportStatus::CREATED_MEDIA_FAILED)
                v->evidence = result.event_id;
            v->next_evidence = now + 1000;
        }
    }
    for (auto it = s.steps.begin(); it != s.steps.end();)
        if (now - it->second.time > continuity)
            it = s.steps.erase(it);
        else
            ++it;

    int hidden = 0, alarms = 0, missing_elsewhere = 0;
    uint64_t longest = 0;
    for (auto &entry : s.visits)
    {
        Visit &v = entry.second;
        if (valid && !observed.count(v.track) && v.approaching && v.missing_at_door && !v.hidden)
        {
            if (v.seen != now && now - v.seen >= confirm)
                v.hidden = true;
        }
        if (v.hidden)
        {
            ++hidden;
            longest = std::max(longest, now - v.seen);
            if (now - v.seen >= timeout)
                ++alarms;
        }
        else if (valid && !observed.count(v.track) && now - v.seen >= confirm)
            ++missing_elsewhere;
    }
    draw_rect(ctx, {8, 8, 382, 64}, {24, 28, 30}, -1, 0.83);
    caption(ctx,
            "可见 " + (valid ? std::to_string(visible) : std::string("--")) + "   在场 " +
                std::to_string(s.visits.size()) + "   机内 " + std::to_string(hidden),
            {16, 34}, green);
    const std::string status = !geometry               ? "请校准平台和盲区"
                               : !valid                ? "推理中断"
                               : alarms                ? "机内滞留超时"
                               : untracked             ? "人员跟踪待确认"
                               : missing_elsewhere     ? "平台人员位置待核实"
                               : visible_without_entry ? "入场记录待核实"
                               : hidden                ? "机内计时"
                                                       : "监测中";
    caption(ctx, status + "   " + std::to_string(longest / 1000) + " / " + std::to_string(timeout / 1000) + " 秒",
            {16, 60}, alarms ? red : amber);
    int row = 0;
    for (auto &entry : s.visits)
    {
        Visit &v = entry.second;
        if (!v.hidden)
            continue;
        const uint64_t elapsed = now - v.seen;
        if (row < 8)
            caption(ctx, "#" + std::to_string(v.id) + " 机内 " + std::to_string(elapsed / 1000) + " 秒",
                    {16, 94 + 24 * row++}, elapsed >= timeout ? red : amber);
        if (!valid || elapsed < timeout || v.reported || now < v.next_alarm)
            continue;
        EventRequest e;
        e.event_type = "machine_blind_timeout";
        e.message = "机床盲区人员滞留超时";
        e.fields = {event_field("visit_id", v.id), event_field("track_id", v.track), event_field("dwell_ms", elapsed),
                    event_field("entry_event_id", v.evidence), event_field("present_count", s.visits.size())};
        v.reported = report_event(ctx, e).accepted();
        v.next_alarm = now + 1000;
    }
    ctx->publish_int("visible_count", valid ? visible : -1);
    ctx->publish_int("present_count", s.visits.size());
    ctx->publish_int("hidden_count", hidden);
    ctx->publish_int("alarm_count", alarms);
    ctx->publish_bool("observation_valid", valid);
}
} // namespace machine_presence
