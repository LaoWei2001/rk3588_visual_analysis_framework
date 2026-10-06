#include "logic/core/logic_common.h"
#include "rules.h"
#include "writer.h"
#include "cJSON.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
uint64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct State {
    std::vector<dataset::Rule> rules;
    std::vector<std::string> labels;
    ChannelModelConfig model;
    std::string error, rules_json, session;
    std::unique_ptr<dataset::Gate> gate;
    std::unique_ptr<dataset::Writer> writer;
    uint64_t matched = 0, last_log = 0;
};
std::string save_directory(ChannelContext* ctx) {
    const std::string configured = ctx->param_string("save_dir");
    if (!configured.empty()) return configured;
    if (const char* configured = getenv("DATASET_STORE_DIR")) return configured;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) throw std::runtime_error("无法确定样本缓存路径");
    std::string path(cwd);
    return "/userdata/rk3588_dataset_samples/" + path.substr(path.find_last_of('/') + 1);
}
std::shared_ptr<State> initialize(ChannelContext* ctx) {
    auto state = std::make_shared<State>();
    try {
        if (!ctx->config) throw std::runtime_error("没有通道配置");
        std::vector<ChannelModelConfig> models;
        for (const auto& model : ctx->config->models) if (model.enable) models.push_back(model);
        // 框架多模型合并结果尚不携带每个子模型的成功状态；禁止把部分失败当成类别缺失。
        if (models.size() != 1) throw std::runtime_error("采集通道需要且只能启用一个检测模型");
        state->model = models.front();
        if (state->model.id.empty()) state->model.id = "model_0";
        if (state->model.model_type != "yolov8_det" && state->model.model_type != "yolov5" && state->model.model_type != "yolov5_seg")
            throw std::runtime_error("采集逻辑需要带类别表的目标检测模型");
        std::ifstream labels(state->model.label_path);
        if (!labels) throw std::runtime_error("无法读取模型类别文件");
        std::string line;
        std::set<std::string> unique;
        while (std::getline(labels, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue; // 与模型加载器保持相同编号规则
            if (!unique.insert(line).second) throw std::runtime_error("类别表存在重复名称");
            state->labels.push_back(line);
        }
        if (state->labels.empty()) throw std::runtime_error("模型类别文件为空");
        state->rules_json = ctx->param_json("rules");
        state->rules = dataset::parse_rules(state->rules_json, state->labels, state->model.detect_classes, state->model.obj_thresh);
        state->gate.reset(new dataset::Gate(state->rules.size()));
        state->session = std::to_string(ctx->unix_ms) + "_" + std::to_string(getpid()) + "_" + std::to_string(steady_ms());
        state->writer.reset(new dataset::Writer(save_directory(ctx) + "/ch" + std::to_string(ctx->chnId), static_cast<int>(ctx->param_int("jpeg_quality")),
                              ctx->param_int("max_store_mb") * 1024ULL * 1024, ctx->param_int("max_samples")));
    } catch (const std::exception& error) { state->error = error.what(); }
    return state;
}
std::string json_text(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    if (!text) { cJSON_Delete(json); throw std::runtime_error("采集元数据序列化失败"); }
    std::string value(text); cJSON_free(text); cJSON_Delete(json); return value;
}
dataset::Sample make_sample(ChannelContext* ctx, State& state, const std::vector<size_t>& hits) {
    const cv::Mat* image = ctx->source_frame();
    if (!image || image->empty()) throw std::runtime_error("无法读取同帧原始画面");
    dataset::Sample sample;
    sample.id = state.session + "_ch" + std::to_string(ctx->chnId) + "_f" + std::to_string(ctx->frame_id);
    sample.image = image->clone();
    sample.with_annotation = ctx->param_bool("save_prediction");
    sample.with_metadata = ctx->param_bool("save_metadata");
    if (!sample.with_annotation && !sample.with_metadata) return sample;
    for (const auto& label : state.labels) sample.labels += label + "\n";
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", sample.id.c_str());
    cJSON_AddStringToObject(root, "annotation_source", "model_prediction_requires_review");
    cJSON_AddNumberToObject(root, "channel_id", ctx->chnId);
    cJSON_AddNumberToObject(root, "frame_id", ctx->frame_id);
    cJSON_AddNumberToObject(root, "unix_ms", ctx->unix_ms);
    cJSON_AddNumberToObject(root, "steady_ms", ctx->timestamp_ms);
    cJSON_AddNumberToObject(root, "width", image->cols);
    cJSON_AddNumberToObject(root, "height", image->rows);
    cJSON_AddStringToObject(root, "model_id", state.model.id.c_str());
    cJSON_AddStringToObject(root, "model_type", state.model.model_type.c_str());
    cJSON_AddStringToObject(root, "model_version", state.model.version.c_str());
    const auto basename = [](const std::string& path) { return path.substr(path.find_last_of('/') + 1); };
    cJSON_AddStringToObject(root, "model_filename", basename(state.model.model_path).c_str());
    cJSON_AddStringToObject(root, "label_filename", basename(state.model.label_path).c_str());
    auto inference_polygon = cJSON_AddArrayToObject(root, "inference_polygon_normalized");
    for (const auto& point : ctx->config->inference_roi.polygon) {
        auto xy = cJSON_CreateArray();
        cJSON_AddItemToArray(xy, cJSON_CreateNumber(point.first));
        cJSON_AddItemToArray(xy, cJSON_CreateNumber(point.second));
        cJSON_AddItemToArray(inference_polygon, xy);
    }
    auto roi_polygon = cJSON_AddArrayToObject(root, "count_roi_polygon_canonical");
    const auto roi = ctx->roi_by_name(ctx->param_string("roi_name").c_str());
    if (roi) for (const auto& point : roi->polygon) {
        auto xy = cJSON_CreateArray();
        cJSON_AddItemToArray(xy, cJSON_CreateNumber(point.x));
        cJSON_AddItemToArray(xy, cJSON_CreateNumber(point.y));
        cJSON_AddItemToArray(roi_polygon, xy);
    }
    cJSON_AddNumberToObject(root, "model_output_threshold", state.model.obj_thresh);
    cJSON_AddStringToObject(root, "inference_region_mode", ctx->config->inference_roi.mode.c_str());
    cJSON_AddStringToObject(root, "roi_name", ctx->param_string("roi_name").c_str());
    cJSON_AddStringToObject(root, "roi_anchor", ctx->param_string("roi_anchor").c_str());
    cJSON_AddItemToObject(root, "rules", cJSON_Parse(state.rules_json.c_str()));
    auto matched = cJSON_AddArrayToObject(root, "matched_rules");
    for (size_t i : hits) {
        auto rule = cJSON_CreateObject();
        cJSON_AddStringToObject(rule, "id", state.rules[i].id.c_str());
        cJSON_AddStringToObject(rule, "name", state.rules[i].name.c_str());
        cJSON_AddItemToArray(matched, rule);
    }
    auto classes = cJSON_AddArrayToObject(root, "classes");
    for (const auto& label : state.labels) cJSON_AddItemToArray(classes, cJSON_CreateString(label.c_str()));
    auto detections = cJSON_AddArrayToObject(root, "detections");
    std::ostringstream annotation; annotation << std::fixed << std::setprecision(6);
    // 推理管线已将不同输入尺寸/推理 ROI 统一映射到 canonical 坐标。
    const double width = inference_get_input_w(), height = inference_get_input_h();
    if (width <= 0 || height <= 0) { cJSON_Delete(root); throw std::runtime_error("检测坐标尺寸无效"); }
    cJSON_AddNumberToObject(root, "detection_width", width);
    cJSON_AddNumberToObject(root, "detection_height", height);
    for (const auto& d : *ctx->results) {
        if (d.model_id != state.model.id || d.frame_id != ctx->frame_id || d.class_id < 0 ||
            static_cast<size_t>(d.class_id) >= state.labels.size()) continue;
        const double x1 = std::max(0.0, std::min(1.0, d.box.x / width));
        const double y1 = std::max(0.0, std::min(1.0, d.box.y / height));
        const double x2 = std::max(0.0, std::min(1.0, (d.box.x + d.box.width) / width));
        const double y2 = std::max(0.0, std::min(1.0, (d.box.y + d.box.height) / height));
        if (x2 <= x1 || y2 <= y1) continue;
        auto item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", d.label.c_str());
        cJSON_AddNumberToObject(item, "class_id", d.class_id);
        cJSON_AddNumberToObject(item, "score", d.score);
        auto box = cJSON_AddArrayToObject(item, "box_xywh_source");
        for (double v : {x1 * image->cols, y1 * image->rows, (x2 - x1) * image->cols, (y2 - y1) * image->rows})
            cJSON_AddItemToArray(box, cJSON_CreateNumber(v));
        cJSON_AddItemToArray(detections, item);
        annotation << d.class_id << ' ' << (x1 + x2) / 2 << ' ' << (y1 + y2) / 2 << ' ' << x2 - x1 << ' ' << y2 - y1 << '\n';
    }
    sample.annotation = annotation.str();
    if (sample.with_metadata) sample.metadata = json_text(root);
    else cJSON_Delete(root);
    return sample;
}
static void logic_dataset_collector(ChannelContext* ctx) {
    if (!ctx || !ctx->state || !ctx->config) return;
    if (!ctx->param_bool("enabled")) {
        if (*ctx->state) {
            auto state = std::static_pointer_cast<State>(*ctx->state);
            if (state->gate) state->gate->reset();
        }
        ctx->publish_bool("collector_active", false); ctx->publish_string("collector_status", "采集已关闭"); return;
    }
    if (!*ctx->state) *ctx->state = initialize(ctx);
    auto& state = *std::static_pointer_cast<State>(*ctx->state);
    std::string status = state.error;
    bool valid = status.empty() && ctx->infer_enabled && ctx->inference_valid && ctx->results;
    const uint64_t now = steady_ms(), gap = static_cast<uint64_t>(ctx->param_float("max_gap_sec") * 1000);
    if (valid && (!ctx->timestamp_ms || ctx->timestamp_ms > now || now - ctx->timestamp_ms > gap)) {
        valid = false; status = "推理帧过期，暂停采集";
    }
    if (!valid) {
        if (state.gate) state.gate->reset();
        if (status.empty()) status = "等待有效推理结果";
    } else {
        const std::string roi_name = ctx->param_string("roi_name");
        const RoiZone* roi = roi_name.empty() ? nullptr : ctx->roi_by_name(roi_name.c_str());
        if (!roi_name.empty() && (!roi || roi->polygon.size() < 3)) { valid = false; status = "找不到有效的采集 ROI：" + roi_name; state.gate->reset(); }
        else {
            const bool foot_anchor = ctx->param_string("roi_anchor") == "foot";
            std::vector<dataset::Detection> detections;
            for (const auto& d : *ctx->results) {
                if (d.model_id != state.model.id || d.frame_id != ctx->frame_id) continue;
                cv::Point point = d.box_center();
                if (foot_anchor) point.y = d.box.y + d.box.height;
                if (roi && cv::pointPolygonTest(roi->polygon, point, false) < 0) continue;
                detections.push_back({d.label, d.score});
            }
            std::vector<bool> matches;
            for (const auto& rule : state.rules) matches.push_back(rule.condition.matches(detections));
            auto hits = state.gate->update(ctx->frame_id, ctx->timestamp_ms, matches,
                    ctx->param_float("confirm_sec") * 1000, ctx->param_float("interval_sec") * 1000,
                    gap, ctx->param_string("trigger_mode") == "on_enter");
            if (!hits.empty()) {
                ++state.matched;
                if (state.writer->available(static_cast<size_t>(ctx->src_width) * ctx->src_height * 3)) {
                    try { state.writer->enqueue(make_sample(ctx, state, hits)); state.error.clear(); }
                    catch (const std::exception& error) { status = error.what(); state.writer->note_skipped(); }
                } else { state.writer->note_skipped(); }
            }
            if (status.empty()) status = state.writer->error();
            if (status.empty()) status = state.writer->saved() >= static_cast<uint64_t>(ctx->param_int("max_samples"))
                                        ? "本次采集达到数量上限" : "采集中";
        }
    }
    if (!status.empty() && status != "采集中" && now - state.last_log >= 10000) {
        state.last_log = now; fprintf(stderr, "[DatasetCollector][ch%02d] %s\n", ctx->chnId, status.c_str());
    }
    ctx->publish_bool("collector_active", valid);
    ctx->publish_string("collector_status", status);
    ctx->publish_int("collector_saved", state.writer ? state.writer->saved() : 0);
    ctx->publish_int("collector_pending", state.writer ? state.writer->pending() : 0);
    ctx->publish_int("collector_matched", state.matched);
    ctx->publish_int("collector_skipped", state.writer ? state.writer->skipped() : 0);
    std::string text = "采集：" + status + " · 已保存 " + std::to_string(state.writer ? state.writer->saved() : 0);
    draw_text(ctx, text.c_str(), cv::Point(12, 28), cv::Scalar(0, 230, 255), 0.6, 1);
}
}
REGISTER_LOGIC(logic_dataset_collector);
