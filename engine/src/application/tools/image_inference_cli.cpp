#include "image_inference_cli.h"
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <inference.h>
#include <iomanip>
#include <json.h>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

namespace
{
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
volatile std::sig_atomic_t cancelled = 0;
void cancel(int)
{
    cancelled = 1;
}
Json object()
{
    return Json(cJSON_CreateObject(), cJSON_Delete);
}
cJSON *field(const cJSON *value, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(value, key);
}
std::string string(const cJSON *value, const char *key, const std::string &fallback = "")
{
    const cJSON *item = field(value, key);
    if (!item)
        return fallback;
    if (!cJSON_IsString(item))
        throw std::invalid_argument(std::string(key) + " must be a string");
    return item->valuestring;
}
double number(const cJSON *value, const char *key, double fallback)
{
    const cJSON *item = field(value, key);
    if (!item)
        return fallback;
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble))
        throw std::invalid_argument(std::string(key) + " must be a finite number");
    return item->valuedouble;
}
void text(cJSON *value, const char *key, const std::string &s)
{
    cJSON_AddStringToObject(value, key, s.c_str());
}
void write_json(const std::string &path, const cJSON *value)
{
    char *encoded = cJSON_PrintUnformatted(value);
    if (!encoded)
        throw std::runtime_error("JSON encoding failed");
    const std::string content(encoded);
    cJSON_free(encoded);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        file << content << '\n';
        file.close();
        if (!file)
            throw std::runtime_error("Cannot write result file (check disk space)");
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
        throw std::runtime_error("Cannot publish result file");
}
void mkdir_checked(const std::string &path)
{
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("Cannot create output directory: " + path);
    struct stat st
    {
    };
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        throw std::runtime_error("Output path is not a directory");
}
class StorageFull : public std::runtime_error
{
public:
    StorageFull() : std::runtime_error("存储空间不足：需保留超过 100 MB，请清理空间后继续") {}
};
void require_space(const std::string &path, size_t incoming = 0)
{
    const auto parent = path.substr(0, path.find_last_of('/'));
    struct statvfs info{};
    if (::statvfs(parent.c_str(), &info) != 0)
        throw std::runtime_error("Cannot check image storage space");
    const uint64_t free = static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
    constexpr uint64_t reserve = 100ULL * 1024 * 1024;
    // Include allocation rounding so a final partial block also preserves the reserve.
    const uint64_t bytes = ((incoming + info.f_frsize - 1) / info.f_frsize) * info.f_frsize;
    if (free <= reserve || bytes >= free - reserve)
        throw StorageFull();
}
void image(const std::string &path, const cv::Mat &pixels)
{
    std::vector<unsigned char> encoded;
    if (!cv::imencode(path.substr(path.find_last_of('.')), pixels, encoded))
        throw std::runtime_error("Cannot encode image: " + path);
    require_space(path, encoded.size());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(encoded.data()), encoded.size());
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write image: " + path);
}

vision::ImageInferenceOptions options(const cJSON *job)
{
    vision::ImageInferenceOptions out;
    const cJSON *models = field(job, "models");
    if (!cJSON_IsArray(models))
        throw std::invalid_argument("models must be an array");
    const cJSON *item;
    cJSON_ArrayForEach(item, models)
    {
        if (!cJSON_IsObject(item))
            throw std::invalid_argument("Each model must be an object");
        ChannelModelConfig model;
        model.id = string(item, "id");
        const auto enabled = field(item, "enable");
        if (enabled && !cJSON_IsBool(enabled))
            throw std::invalid_argument("enable must be boolean");
        model.enable = !enabled || cJSON_IsTrue(enabled);
        model.model_type = string(item, "model_type");
        model.model_path = string(item, "model_path");
        model.label_path = string(item, "label_path");
        model.obj_thresh = number(item, "obj_thresh", .3);
        model.nms_thresh = number(item, "nms_thresh", .45);
        const double core = number(item, "npu_core", -1);
        if (core != std::floor(core) || core < -1 || core > 2)
            throw std::invalid_argument("Invalid NPU core");
        model.npu_core = static_cast<int>(core);
        const cJSON *classes = field(item, "detect_classes");
        if (classes && !cJSON_IsArray(classes))
            throw std::invalid_argument("detect_classes must be an array");
        const cJSON *label;
        cJSON_ArrayForEach(label, classes)
        {
            if (!cJSON_IsString(label))
                throw std::invalid_argument("Class labels must be strings");
            model.detect_classes.emplace_back(label->valuestring);
        }
        out.models.push_back(std::move(model));
    }
    if (const auto roi = field(job, "inference_roi"))
    {
        if (!cJSON_IsObject(roi))
            throw std::invalid_argument("inference_roi must be an object");
        out.inference_roi.mode = string(roi, "mode");
        out.inference_roi.resize_mode = string(roi, "resize_mode", "stretch");
        const cJSON *polygon = field(roi, "polygon");
        if (polygon && !cJSON_IsArray(polygon))
            throw std::invalid_argument("ROI polygon must be an array");
        const cJSON *point;
        cJSON_ArrayForEach(point, polygon)
        {
            if (!cJSON_IsArray(point) || cJSON_GetArraySize(point) != 2 ||
                !cJSON_IsNumber(cJSON_GetArrayItem(point, 0)) || !cJSON_IsNumber(cJSON_GetArrayItem(point, 1)))
                throw std::invalid_argument("ROI points must contain x and y");
            out.inference_roi.polygon.emplace_back(cJSON_GetArrayItem(point, 0)->valuedouble,
                                                   cJSON_GetArrayItem(point, 1)->valuedouble);
        }
    }
    return out;
}

void write_depth(const std::string &path, const cv::Mat &depth)
{
    if (depth.type() != CV_32FC1)
        throw std::runtime_error("Depth payload must be CV_32FC1");
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(depth.rows) + ", " +
                         std::to_string(depth.cols) + "), }";
    header.append((64 - ((10 + header.size() + 1) % 64)) % 64, ' ');
    header += '\n';
    require_space(path, 10 + header.size() + depth.total() * sizeof(float));
    std::ofstream file(path, std::ios::binary);
    const unsigned char prefix[] = {0x93,
                                    'N',
                                    'U',
                                    'M',
                                    'P',
                                    'Y',
                                    1,
                                    0,
                                    static_cast<unsigned char>(header.size() & 255),
                                    static_cast<unsigned char>(header.size() >> 8)};
    file.write(reinterpret_cast<const char *>(prefix), sizeof(prefix));
    file << header;
    for (int row = 0; row < depth.rows; ++row)
        file.write(reinterpret_cast<const char *>(depth.ptr<float>(row)), depth.cols * sizeof(float));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write depth payload");
}

cJSON *point_json(double x, double y)
{
    auto point = cJSON_CreateArray();
    cJSON_AddItemToArray(point, cJSON_CreateNumber(x));
    cJSON_AddItemToArray(point, cJSON_CreateNumber(y));
    return point;
}

Json result_json(const vision::FrameInferenceResult &result, const cv::Mat &source, const std::string &directory)
{
    auto root = object();
    cJSON_AddNumberToObject(root.get(), "schema_version", 1);
    cJSON_AddNumberToObject(root.get(), "source_width", result.source_width);
    cJSON_AddNumberToObject(root.get(), "source_height", result.source_height);
    cJSON_AddNumberToObject(root.get(), "coordinate_width", result.coordinate_width);
    cJSON_AddNumberToObject(root.get(), "coordinate_height", result.coordinate_height);
    cJSON_AddNumberToObject(root.get(), "elapsed_ms", result.elapsed_ms);
    text(root.get(), "original", "original.jpg");
    image(directory + "/original.jpg", source);
    cv::Mat drawn = source.clone();
    vision::draw_inference_results(drawn, result);
    image(directory + "/annotated.jpg", drawn);
    text(root.get(), "annotated", "annotated.jpg");
    auto models = cJSON_AddArrayToObject(root.get(), "models");
    for (size_t i = 0; i < result.models.size(); ++i)
    {
        const auto &model = result.models[i];
        auto value = cJSON_CreateObject();
        cJSON_AddItemToArray(models, value);
        text(value, "id", model.model_id);
        text(value, "type", model.model_type);
        cJSON_AddBoolToObject(value, "succeeded", model.succeeded);
        text(value, "error", model.error);
        cJSON_AddNumberToObject(value, "input_width", model.input_width);
        cJSON_AddNumberToObject(value, "input_height", model.input_height);
        cJSON_AddNumberToObject(value, "preprocess_ms", model.timing.preprocess_ms);
        cJSON_AddNumberToObject(value, "infer_ms", model.timing.infer_ms);
        cJSON_AddNumberToObject(value, "postprocess_ms", model.timing.postprocess_ms);
        if (!model.succeeded)
            continue;
        const std::string prefix = "model_" + std::to_string(i);
        drawn = source.clone();
        vision::draw_inference_results(drawn, result, model.model_id);
        image(directory + "/" + prefix + ".jpg", drawn);
        text(value, "annotated", prefix + ".jpg");
        for (const auto &r : result.results)
        {
            if (r.model_id != model.model_id)
                continue;
            if (!r.boxMask.empty())
            {
                image(directory + "/" + prefix + "_mask.png", r.boxMask);
                text(value, "mask", prefix + "_mask.png");
            }
            if (r.is_depth())
            {
                write_depth(directory + "/" + prefix + "_depth.npy", r.depth_map);
                text(value, "depth", prefix + "_depth.npy");
                text(value, "depth_kind", r.is_metric_depth() ? "metric" : "relative_inverse");
            }
        }
    }
    auto objects = cJSON_AddArrayToObject(root.get(), "results");
    for (const auto &r : result.results)
    {
        auto value = cJSON_CreateObject();
        cJSON_AddItemToArray(objects, value);
        text(value, "model_id", r.model_id);
        text(value, "model_type", r.model_type);
        cJSON_AddNumberToObject(value, "model_index", r.model_index);
        text(value, "label", r.label);
        cJSON_AddNumberToObject(value, "class_id", r.class_id);
        cJSON_AddNumberToObject(value, "score", r.score);
        cJSON_AddNumberToObject(value, "track_id", r.track_id);
        cJSON_AddNumberToObject(value, "frame_id", r.frame_id);
        auto box = cJSON_AddObjectToObject(value, "box");
        cJSON_AddNumberToObject(box, "x", r.box.x);
        cJSON_AddNumberToObject(box, "y", r.box.y);
        cJSON_AddNumberToObject(box, "width", r.box.width);
        cJSON_AddNumberToObject(box, "height", r.box.height);
        if (r.has_obb)
        {
            auto corners = cJSON_AddArrayToObject(value, "obb_corners");
            for (const auto &point : r.obb_corners)
                cJSON_AddItemToArray(corners, point_json(point.x, point.y));
        }
        if (r.is_pose())
        {
            text(value, "pose_schema", r.pose_schema == PoseKeypointSchema::Coco17 ? "coco17" : "hand21");
            auto points = cJSON_AddArrayToObject(value, "keypoints");
            for (const auto &point : r.keypoints)
                cJSON_AddItemToArray(points, point_json(point.x, point.y));
            auto scores = cJSON_AddArrayToObject(value, "keypoint_scores");
            for (float score : r.keypoint_scores)
                cJSON_AddItemToArray(scores, cJSON_CreateNumber(score));
        }
        if (!r.text_result.empty())
            text(value, "text", r.text_result);
        if (r.is_depth())
            text(value, "depth_kind", r.is_metric_depth() ? "metric" : "relative_inverse");
    }
    return root;
}
} // namespace

int image_inference_capabilities_cli()
{
    auto root = object();
    cJSON_AddNumberToObject(root.get(), "version", 1);
    cJSON_AddNumberToObject(root.get(), "max_models", 8);
    cJSON_AddNumberToObject(root.get(), "max_pixels", 16777216);
    auto models = cJSON_AddArrayToObject(root.get(), "models");
    for (const auto &model : vision::inference_model_types())
    {
        auto value = cJSON_CreateObject();
        cJSON_AddItemToArray(models, value);
        text(value, "type", model.type);
        text(value, "result_kind", model.result_kind);
        cJSON_AddBoolToObject(value, "requires_labels", model.requires_labels);
    }
    char *encoded = cJSON_PrintUnformatted(root.get());
    std::printf("IMAGE_INFERENCE_CAPABILITIES=%s\n", encoded);
    cJSON_free(encoded);
    return 0;
}

int image_inference_cli(const char *job_path)
{
    setenv("OPENCV_IO_MAX_IMAGE_PIXELS", "16777216", 1);
    cancelled = 0;
    std::signal(SIGINT, cancel);
    std::signal(SIGTERM, cancel);
    std::string progress_path;
    auto progress = object();
    try
    {
        std::ifstream file(job_path, std::ios::binary);
        if (!file)
            throw std::invalid_argument("Cannot open image inference job");
        std::string source((std::istreambuf_iterator<char>(file)), {});
        Json job(cJSON_Parse(source.c_str()), cJSON_Delete);
        if (!job || !cJSON_IsObject(job.get()))
            throw std::invalid_argument("Invalid job JSON");
        const auto settings = options(job.get());
        const auto images = field(job.get(), "images");
        const int count = cJSON_GetArraySize(images);
        if (!cJSON_IsArray(images) || count < 1)
            throw std::invalid_argument("Select at least one image");
        const std::string output = string(job.get(), "output_dir");
        if (output.empty())
            throw std::invalid_argument("output_dir is required");
        mkdir_checked(output);
        std::ifstream previous(output + "/progress.json");
        if (previous.good())
        {
            if (!cJSON_IsTrue(field(job.get(), "resume")))
                throw std::invalid_argument("Output already contains a job");
            std::string saved((std::istreambuf_iterator<char>(previous)), {});
            Json restored(cJSON_Parse(saved.c_str()), cJSON_Delete);
            if (!restored || !cJSON_IsArray(field(restored.get(), "items")) ||
                cJSON_GetArraySize(field(restored.get(), "items")) != count)
                throw std::invalid_argument("Invalid resume progress");
            progress = std::move(restored);
        }
        progress_path = output + "/progress.json";
        auto items = field(progress.get(), "items");
        const bool resuming = items != nullptr;
        if (!items)
            items = cJSON_AddArrayToObject(progress.get(), "items");
        std::vector<const cJSON *> inputs;
        const cJSON *input;
        cJSON_ArrayForEach(input, images)
            inputs.push_back(input);
        std::vector<cJSON *> statuses;
        for (int i = 0; i < count; ++i)
        {
            auto item = resuming ? cJSON_GetArrayItem(items, i) : cJSON_CreateObject();
            statuses.push_back(item);
            if (resuming)
            {
                if (number(item, "index", -1) != i || string(item, "name") != string(inputs[i], "name"))
                    throw std::invalid_argument("Resume image list changed");
                const auto state = string(item, "status");
                if (state != "success" && state != "partial" && state != "failed")
                {
                    cJSON_ReplaceItemInObjectCaseSensitive(item, "status", cJSON_CreateString("pending"));
                    cJSON_DeleteItemFromObjectCaseSensitive(item, "error");
                }
            }
            else
            {
                cJSON_AddItemToArray(items, item);
                cJSON_AddNumberToObject(item, "index", i);
                text(item, "name", string(inputs[i], "name"));
                text(item, "status", "pending");
            }
        }
        cJSON_DeleteItemFromObjectCaseSensitive(progress.get(), "status");
        cJSON_DeleteItemFromObjectCaseSensitive(progress.get(), "error");
        text(progress.get(), "status", "loading");
        write_json(progress_path, progress.get());
        cv::setNumThreads(2);
        vision::ImageInferenceSession session(settings);
        int failures = 0;
        cJSON_ReplaceItemInObjectCaseSensitive(progress.get(), "status", cJSON_CreateString("running"));
        for (int i = 0; i < count && !cancelled; ++i)
        {
            auto item = statuses[i];
            const auto state = string(item, "status");
            if (state == "success" || state == "partial" || state == "failed")
            {
                if (state != "success")
                    ++failures;
                continue;
            }
            cJSON_ReplaceItemInObjectCaseSensitive(item, "status", cJSON_CreateString("running"));
            write_json(progress_path, progress.get());
            try
            {
                require_space(output + "/result");
                const std::string path = string(inputs[i], "path");
                cv::Mat pixels = cv::imread(path, cv::IMREAD_COLOR);
                if (pixels.empty())
                    throw std::runtime_error("Cannot decode image");
                if (pixels.total() > 16777216)
                    throw std::runtime_error("Image exceeds 16 megapixels");
                auto result = session.infer(pixels);
                std::ostringstream folder;
                folder << std::setw(6) << std::setfill('0') << i;
                const std::string directory = output + "/" + folder.str();
                mkdir_checked(directory);
                auto details = result_json(result, pixels, directory);
                write_json(directory + "/result.json", details.get());
                text(item, "result", folder.str() + "/result.json");
                cJSON_AddNumberToObject(item, "elapsed_ms", result.elapsed_ms);
                cJSON_ReplaceItemInObjectCaseSensitive(item, "status",
                                                       cJSON_CreateString(result.complete()    ? "success"
                                                                          : result.succeeded() ? "partial"
                                                                                               : "failed"));
                if (!result.complete())
                    ++failures;
            }
            catch (const StorageFull &)
            {
                throw;
            }
            catch (const std::exception &error)
            {
                // A write may exhaust storage after the preflight check.
                require_space(output + "/result");
                ++failures;
                cJSON_ReplaceItemInObjectCaseSensitive(item, "status", cJSON_CreateString("failed"));
                text(item, "error", error.what());
            }
            write_json(progress_path, progress.get());
        }
        for (int i = 0; i < count; ++i)
        {
            auto item = statuses[i];
            if (string(item, "status") == "pending")
                cJSON_ReplaceItemInObjectCaseSensitive(item, "status", cJSON_CreateString("cancelled"));
        }
        cJSON_ReplaceItemInObjectCaseSensitive(progress.get(), "status",
                                               cJSON_CreateString(cancelled  ? "cancelled"
                                                                  : failures ? "completed_with_errors"
                                                                             : "completed"));
        write_json(progress_path, progress.get());
        return cancelled ? 130 : failures ? 1 : 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "[ImageInference] %s\n", error.what());
        if (!progress_path.empty())
        {
            cJSON_DeleteItemFromObjectCaseSensitive(progress.get(), "status");
            text(progress.get(), "status", dynamic_cast<const StorageFull *>(&error) ? "paused_storage"
                                                                                    : cancelled ? "cancelled" : "failed");
            text(progress.get(), "error", error.what());
            try
            {
                write_json(progress_path, progress.get());
            }
            catch (...)
            {
            }
        }
        return cancelled ? 130 : 2;
    }
}
