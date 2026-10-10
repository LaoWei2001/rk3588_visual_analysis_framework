#include "remote_dataset.h"
#include "dataset/rules.h"
#include "runtime/app_ctrl.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <json.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace
{
uint64_t steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
uint64_t unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
const size_t kBytes = 64ULL * 1024 * 1024, kMaxJpeg = 12ULL * 1024 * 1024;
struct Image
{
    std::string id;
    int64_t frame;
    uint64_t timestamp;
    std::vector<unsigned char> jpeg;
};
struct Task
{
    std::string id, revision, rules_json, model_id, roi, status = "等待有效推理结果";
    int channel = -1, quality = 95;
    bool enabled = false, on_enter = false, foot = false, invalid = false;
    uint64_t saved = 0, limit = 0, deadline = 0, matched = 0, skipped = 0, last_frame_at = 0;
    uint64_t interval = 2000, confirm = 0, gap = 2000;
    size_t encoding = 0;
    std::vector<dataset::Rule> rules;
    std::unique_ptr<dataset::Gate> gate;
    std::shared_ptr<const AppRuntimeSnapshot> runtime;
    std::deque<std::shared_ptr<Image>> images;
};
struct Job
{
    std::shared_ptr<Task> task;
    std::shared_ptr<Image> image;
    cv::Mat frame;
    size_t bytes;
};
std::mutex mutex;
std::condition_variable wake;
std::map<int, std::shared_ptr<Task>> tasks;
std::deque<Job> jobs;
size_t raw_bytes = 0, jpeg_bytes = 0;
std::atomic<bool> running{false};
std::atomic<unsigned> active_mask{0};
std::thread encoder, server;
int server_fd = -1;
std::string socket_path, boot;

std::string text(const cJSON *o, const char *key, const std::string &fallback = "")
{
    const auto v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v)
        return fallback;
    if (!cJSON_IsString(v))
        throw std::runtime_error(std::string(key) + " 必须是字符串");
    return v->valuestring;
}
double number(const cJSON *o, const char *key, double fallback, double low, double high, bool integer = false)
{
    const auto v = cJSON_GetObjectItemCaseSensitive(o, key);
    const double n = v ? v->valuedouble : fallback;
    if ((v && !cJSON_IsNumber(v)) || !std::isfinite(n) || n < low || n > high || (integer && std::floor(n) != n))
        throw std::runtime_error(std::string(key) + " 数值无效");
    return n;
}
bool boolean(const cJSON *o, const char *key, bool fallback)
{
    auto v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v)
        return fallback;
    if (!cJSON_IsBool(v))
        throw std::runtime_error(std::string(key) + " 必须是开关");
    return cJSON_IsTrue(v);
}
std::string stringify(cJSON *o)
{
    char *p = cJSON_PrintUnformatted(o);
    std::string result = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(o);
    return result;
}
void refresh_mask()
{
    unsigned mask = 0;
    for (const auto &entry : tasks)
        if (entry.second->enabled)
            mask |= 1U << entry.first;
    active_mask.store(mask);
}
void validate(Task &task, const std::shared_ptr<const AppRuntimeSnapshot> &runtime)
{
    const auto config = app_ctrl_runtime_channel_config(runtime, task.channel);
    if (!config || !config->enable)
        throw std::runtime_error("采集通道不存在或已关闭");
    std::vector<ChannelModelConfig> models;
    for (const auto &model : config->models)
        if (model.enable)
            models.push_back(model);
    if (models.size() != 1)
        throw std::runtime_error("第一版每个采集通道需要且只能启用一个检测模型");
    const auto &model = models.front();
    if (model.model_type != "yolov8_det" && model.model_type != "yolov5" && model.model_type != "yolov5_seg")
        throw std::runtime_error("采集需要目标检测模型");
    std::ifstream input(model.label_path);
    if (!input)
        throw std::runtime_error("无法读取模型类别表");
    std::vector<std::string> labels;
    std::set<std::string> seen;
    std::string label;
    while (std::getline(input, label))
    {
        if (!label.empty() && label.back() == '\r')
            label.pop_back();
        if (label.empty())
            continue;
        if (!seen.insert(label).second)
            throw std::runtime_error("类别表包含重复名称");
        labels.push_back(label);
    }
    if (labels.empty())
        throw std::runtime_error("模型类别表为空");
    task.rules = dataset::parse_rules(task.rules_json, labels, model.detect_classes, model.obj_thresh);
    task.model_id = model.id.empty() ? "model_0" : model.id;
    task.gate.reset(new dataset::Gate(task.rules.size()));
    task.runtime = runtime;
}
bool expired(const Task &task)
{
    return task.deadline && unix_ms() >= task.deadline;
}
std::string status(const Task &task)
{
    if (!task.enabled)
        return "已暂停";
    if (expired(task))
        return "已达到运行时长";
    if (task.limit && task.saved >= task.limit)
        return "已达到图片上限";
    if (task.invalid)
        return task.status;
    if (task.images.size() + task.encoding >= 8 || jpeg_bytes + raw_bytes >= kBytes)
        return "缓存已满，等待电脑接收";
    if (!task.last_frame_at || steady_ms() - task.last_frame_at > task.gap)
        return "等待有效推理结果";
    return task.status;
}
void encode_loop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [] { return !running.load() || !jobs.empty(); });
            if (!running.load())
                return;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        bool ok = false;
        try
        {
            ok = cv::imencode(".jpg", job.frame, job.image->jpeg, {cv::IMWRITE_JPEG_QUALITY, job.task->quality});
        }
        catch (...)
        {
        }
        std::lock_guard<std::mutex> lock(mutex);
        raw_bytes -= job.bytes;
        --job.task->encoding;
        const size_t bytes = job.image->jpeg.size();
        auto current = tasks.find(job.task->channel);
        if (ok && bytes && bytes <= kMaxJpeg && jpeg_bytes + raw_bytes + bytes <= kBytes && current != tasks.end() &&
            current->second == job.task)
        {
            jpeg_bytes += bytes;
            job.task->images.push_back(job.image);
        }
        else
        {
            ++job.task->skipped;
            job.task->status = "图片编码失败或缓存已满";
        }
    }
}
bool send_bytes(int fd, const void *data, size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size)
    {
        const ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        size -= n;
    }
    return true;
}
void serve_loop()
{
    while (running.load())
    {
        pollfd pfd{server_fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0)
            continue;
        int fd = accept(server_fd, nullptr, nullptr);
        if (fd < 0)
            continue;
        timeval timeout{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string request;
        char buffer[4096];
        while (request.size() <= 65536 && request.find('\n') == std::string::npos)
        {
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
                break;
            request.append(buffer, n);
        }
        if (request.size() <= 65536 && request.find('\n') != std::string::npos)
        {
            std::vector<unsigned char> jpeg;
            const std::string header = remote_dataset_command(request, jpeg) + "\n";
            if (send_bytes(fd, header.data(), header.size()) && !jpeg.empty())
                send_bytes(fd, jpeg.data(), jpeg.size());
        }
        close(fd);
    }
}
} // namespace

bool remote_dataset_active(int channel)
{
    return channel >= 0 && channel < 32 && (active_mask.load() & (1U << channel));
}

void remote_dataset_observe(ChannelContext &ctx, const std::shared_ptr<const AppRuntimeSnapshot> &runtime)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tasks.find(ctx.chnId);
    if (found == tasks.end())
        return;
    auto task = found->second;
    if (!task->enabled || expired(*task) ||
        (task->limit && task->saved + task->encoding + task->images.size() >= task->limit))
        return;
    if (!ctx.infer_enabled || !ctx.inference_valid || !ctx.results || !ctx.timestamp_ms ||
        ctx.timestamp_ms > steady_ms() || steady_ms() - ctx.timestamp_ms > task->gap)
    {
        task->gate->reset();
        task->status = "等待有效推理结果";
        return;
    }
    try
    {
        if (task->runtime != runtime)
            validate(*task, runtime);
        const RoiZone *roi = task->roi.empty() ? nullptr : ctx.roi_by_name(task->roi.c_str());
        if (!task->roi.empty() && (!roi || roi->polygon.size() < 3))
            throw std::runtime_error("采集 ROI 不存在或无效");
        std::vector<dataset::Detection> detections;
        for (const auto &d : *ctx.results)
        {
            if (d.frame_id != ctx.frame_id || d.model_id != task->model_id)
                continue;
            cv::Point anchor = d.box_center();
            if (task->foot)
                anchor.y = d.box.y + d.box.height;
            bool scope = !roi || cv::pointPolygonTest(roi->polygon, anchor, false) >= 0;
            detections.emplace_back(d.label, d.score, d.box.x, d.box.y, d.box.width, d.box.height, scope);
        }
        task->last_frame_at = steady_ms();
        task->status = "采集中";
        task->invalid = false;
        std::vector<bool> matches;
        for (const auto &rule : task->rules)
            matches.push_back(rule.condition.matches(detections));
        auto hits = task->gate->update(ctx.frame_id, ctx.timestamp_ms, matches, task->confirm, task->interval,
                                       task->gap, task->on_enter);
        if (hits.empty())
            return;
        ++task->matched;
        const size_t estimate = static_cast<size_t>(std::max(0, ctx.src_width)) * std::max(0, ctx.src_height) * 3;
        if (task->encoding + task->images.size() >= 8 || jobs.size() >= 2 || estimate > kBytes ||
            raw_bytes + jpeg_bytes + estimate > kBytes)
        {
            ++task->skipped;
            task->status = "缓存已满，等待电脑接收";
            return;
        }
        const auto frame = ctx.source_frame();
        if (!frame || frame->empty())
            throw std::runtime_error("无法读取同帧原始图片");
        const size_t bytes = frame->total() * frame->elemSize();
        if (raw_bytes + jpeg_bytes + bytes > kBytes)
        {
            ++task->skipped;
            return;
        }
        auto image = std::make_shared<Image>();
        image->id = task->id + "_" + boot + "_ch" + std::to_string(ctx.chnId) + "_f" + std::to_string(ctx.frame_id) +
                    "_" + std::to_string(task->matched);
        image->frame = ctx.frame_id;
        image->timestamp = ctx.unix_ms;
        jobs.push_back({task, image, frame->clone(), bytes});
        raw_bytes += bytes;
        ++task->encoding;
        wake.notify_one();
    }
    catch (const std::exception &e)
    {
        task->status = e.what();
        task->invalid = true;
        task->gate->reset();
    }
}

std::string remote_dataset_command(const std::string &request, std::vector<unsigned char> &jpeg)
{
    jpeg.clear();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(request.c_str()), cJSON_Delete);
    auto response = cJSON_CreateObject();
    try
    {
        if (!root || !cJSON_IsObject(root.get()))
            throw std::runtime_error("无效的采集请求");
        const auto op = text(root.get(), "op");
        std::lock_guard<std::mutex> lock(mutex);
        if (op == "configure")
        {
            auto task = std::make_shared<Task>();
            task->channel = number(root.get(), "channel_id", -1, 0, std::min(31, MAX_CHANNEL_NUM - 1), true);
            task->id = text(root.get(), "task_id");
            task->revision = text(root.get(), "revision");
            if (task->id.empty() || task->id.size() > 80 || task->revision.empty())
                throw std::runtime_error("采集任务编号无效");
            auto old = tasks.find(task->channel);
            if (old != tasks.end() && old->second->id == task->id && old->second->revision == task->revision)
            {
                const bool enabled = boolean(root.get(), "enabled", false);
                if (old->second->enabled != enabled)
                    old->second->gate->reset();
                old->second->enabled = enabled;
                old->second->saved =
                    std::max(old->second->saved, static_cast<uint64_t>(number(root.get(), "saved", 0, 0, 1e12, true)));
                old->second->deadline = number(root.get(), "deadline_ms", 0, 0, 1e15, true);
                refresh_mask();
            }
            else
            {
                if (old != tasks.end() && (!old->second->images.empty() || old->second->encoding))
                    throw std::runtime_error("请先接收完待传图片再修改条件");
                task->enabled = boolean(root.get(), "enabled", false);
                task->saved = number(root.get(), "saved", 0, 0, 1e12, true);
                task->limit = number(root.get(), "max_samples", 0, 0, 1e9, true);
                task->deadline = number(root.get(), "deadline_ms", 0, 0, 1e15, true);
                task->interval = number(root.get(), "interval_sec", 2, .5, 86400) * 1000;
                task->confirm = number(root.get(), "confirm_sec", 0, 0, 60) * 1000;
                task->quality = number(root.get(), "jpeg_quality", 95, 60, 100, true);
                task->roi = text(root.get(), "roi_name");
                const auto anchor = text(root.get(), "roi_anchor", "center"),
                           trigger = text(root.get(), "trigger_mode", "periodic");
                if (anchor != "foot" && anchor != "center")
                    throw std::runtime_error("ROI定位点无效");
                if (trigger != "periodic" && trigger != "on_enter")
                    throw std::runtime_error("采集触发方式无效");
                task->foot = anchor == "foot";
                task->on_enter = trigger == "on_enter";
                auto rules = cJSON_GetObjectItemCaseSensitive(root.get(), "rules");
                if (!cJSON_IsArray(rules))
                    throw std::runtime_error("采集条件必须是数组");
                char *p = cJSON_PrintUnformatted(rules);
                task->rules_json = p ? p : "[]";
                cJSON_free(p);
                validate(*task, app_ctrl_get_runtime_snapshot());
                tasks[task->channel] = task;
                refresh_mask();
            }
        }
        else if (op == "ack")
        {
            const auto id = text(root.get(), "task_id"), image_id = text(root.get(), "sample_id");
            for (auto &entry : tasks)
                if (entry.second->id == id)
                {
                    auto &task = *entry.second;
                    task.saved =
                        std::max(task.saved, static_cast<uint64_t>(number(root.get(), "saved", 0, 0, 1e12, true)));
                    for (auto it = task.images.begin(); it != task.images.end(); ++it)
                        if ((*it)->id == image_id)
                        {
                            jpeg_bytes -= (*it)->jpeg.size();
                            task.images.erase(it);
                            break;
                        }
                }
        }
        else if (op == "image")
        {
            const auto id = text(root.get(), "task_id"), image_id = text(root.get(), "sample_id");
            bool found = false;
            for (const auto &entry : tasks)
                if (entry.second->id == id)
                    for (const auto &image : entry.second->images)
                        if (image->id == image_id)
                        {
                            jpeg = image->jpeg;
                            found = true;
                        }
            if (!found)
                throw std::runtime_error("图片已释放或运行程序已重启");
            cJSON_AddNumberToObject(response, "bytes", jpeg.size());
        }
        else if (op == "remove")
        {
            const auto id = text(root.get(), "task_id");
            for (auto it = tasks.begin(); it != tasks.end();)
            {
                if (it->second->id != id)
                {
                    ++it;
                    continue;
                }
                if (!it->second->images.empty() || it->second->encoding)
                    throw std::runtime_error("待传图片未接收完成，不能删除任务");
                it = tasks.erase(it);
            }
            refresh_mask();
        }
        else if (op != "status")
            throw std::runtime_error("未知采集操作");
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddStringToObject(response, "boot", boot.c_str());
        auto list = cJSON_AddArrayToObject(response, "tasks");
        for (const auto &entry : tasks)
        {
            const auto &task = *entry.second;
            auto item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "task_id", task.id.c_str());
            cJSON_AddStringToObject(item, "revision", task.revision.c_str());
            cJSON_AddBoolToObject(item, "enabled", task.enabled);
            cJSON_AddNumberToObject(item, "channel_id", task.channel);
            cJSON_AddStringToObject(item, "status", status(task).c_str());
            cJSON_AddNumberToObject(item, "pending", task.images.size() + task.encoding);
            cJSON_AddNumberToObject(item, "saved", task.saved);
            cJSON_AddNumberToObject(item, "matched", task.matched);
            cJSON_AddNumberToObject(item, "skipped", task.skipped);
            if (!task.images.empty())
            {
                const auto &image = *task.images.front();
                auto next = cJSON_AddObjectToObject(item, "next");
                cJSON_AddStringToObject(next, "sample_id", image.id.c_str());
                cJSON_AddNumberToObject(next, "bytes", image.jpeg.size());
                cJSON_AddNumberToObject(next, "frame_id", image.frame);
                cJSON_AddNumberToObject(next, "unix_ms", image.timestamp);
            }
            cJSON_AddItemToArray(list, item);
        }
    }
    catch (const std::exception &e)
    {
        cJSON_AddBoolToObject(response, "ok", false);
        cJSON_AddStringToObject(response, "message", e.what());
    }
    return stringify(response);
}

int remote_dataset_init(const std::string &path)
{
    if (running.load())
        return 0;
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return -1;
    socket_path = path;
    boot = std::to_string(unix_ms()) + "_" + std::to_string(getpid());
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(path.begin(), path.end(), address.sun_path);
    unlink(path.c_str());
    if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(server_fd, 8))
    {
        close(server_fd);
        server_fd = -1;
        unlink(path.c_str());
        return -1;
    }
    chmod(path.c_str(), 0600);
    running.store(true);
    encoder = std::thread(encode_loop);
    server = std::thread(serve_loop);
    return 0;
}
void remote_dataset_deinit()
{
    running.store(false);
    wake.notify_all();
    if (encoder.joinable())
        encoder.join();
    if (server.joinable())
        server.join();
    if (server_fd >= 0)
        close(server_fd);
    server_fd = -1;
    if (!socket_path.empty())
        unlink(socket_path.c_str());
    std::lock_guard<std::mutex> lock(mutex);
    tasks.clear();
    jobs.clear();
    raw_bytes = jpeg_bytes = 0;
    active_mask.store(0);
}
