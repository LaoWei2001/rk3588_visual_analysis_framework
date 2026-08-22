#include "logic_control.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "runtime/app_ctrl.h"
#include "logic/core/global_logic.h"
#include "third_party/json/cJSON.h"

namespace
{

constexpr int kMaxChannels = APP_CTRL_MAX_CAPTURERS;
constexpr int kMaxQueuePerChannel = 64;
constexpr int kMaxRequestBytes = 64 * 1024;

std::mutex g_action_mutex[kMaxChannels];
std::deque<LogicAction> g_action_queue[kMaxChannels];
std::mutex g_global_action_mutex;
std::unordered_map<std::string, std::deque<LogicAction>> g_global_action_queues;
std::atomic<bool> g_running{false};
std::thread g_server_thread;
int g_server_fd = -1;
std::string g_socket_path;

std::string build_default_socket_path()
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/rk3588_logic_control_%d.sock", static_cast<int>(getpid()));
    return std::string(buf);
}

void close_fd(int &fd)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

std::string make_response(bool ok, const char *request_id, int channel_id, const char *action, const char *logic_name,
                          const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return "{\"ok\":false,\"message\":\"oom\"}\n";

    cJSON_AddBoolToObject(root, "ok", ok ? 1 : 0);
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddNumberToObject(root, "channel_id", channel_id);
    cJSON_AddStringToObject(root, "action", action ? action : "");
    cJSON_AddStringToObject(root, "logic_name", logic_name ? logic_name : "");
    cJSON_AddStringToObject(root, "message", message ? message : "");

    char *text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{\"ok\":false,\"message\":\"encode failed\"}";
    if (text)
        cJSON_free(text);
    cJSON_Delete(root);
    out.push_back('\n');
    return out;
}

void send_all(int fd, const std::string &text)
{
    const char *p = text.c_str();
    size_t left = text.size();
    while (left > 0)
    {
        const ssize_t n = send(fd, p, left, 0);
        if (n <= 0)
            return;
        p += n;
        left -= static_cast<size_t>(n);
    }
}

std::string current_logic_name(int channel_id)
{
    return app_ctrl_get_logic_name(channel_id);
}

std::string current_global_logic_name(const std::string &instance_id)
{
    const auto runtime = app_ctrl_get_runtime_snapshot();
    if (!runtime)
        return {};
    for (const GlobalLogicConfig &config : runtime->config.global_logics)
        if (config.enable && config.instance_id == instance_id)
            return config.logic;
    return {};
}

void enqueue_action(const LogicAction &action, int channel_id)
{
    std::lock_guard<std::mutex> lock(g_action_mutex[channel_id]);
    auto &queue = g_action_queue[channel_id];
    if (static_cast<int>(queue.size()) >= kMaxQueuePerChannel)
        queue.pop_front();
    queue.push_back(action);
}

void enqueue_global_action(const LogicAction &action, const std::string &instance_id)
{
    std::lock_guard<std::mutex> lock(g_global_action_mutex);
    auto &queue = g_global_action_queues[instance_id];
    if (static_cast<int>(queue.size()) >= kMaxQueuePerChannel)
        queue.pop_front();
    queue.push_back(action);
}

void handle_client(int client_fd)
{
    char buffer[kMaxRequestBytes + 1];
    const ssize_t n = recv(client_fd, buffer, kMaxRequestBytes, 0);
    if (n <= 0)
        return;
    buffer[n] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    if (!root)
    {
        send_all(client_fd, make_response(false, "", -1, "", "", "invalid json"));
        return;
    }

    const cJSON *req_id_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *scope_item = cJSON_GetObjectItemCaseSensitive(root, "scope");
    const cJSON *instance_item = cJSON_GetObjectItemCaseSensitive(root, "instance_id");
    const cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(root, "channel_id");
    const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(root, "payload");

    const char *req_id = cJSON_IsString(req_id_item) ? req_id_item->valuestring : "";
    const char *scope = cJSON_IsString(scope_item) ? scope_item->valuestring : "channel";
    const char *instance_id = cJSON_IsString(instance_item) ? instance_item->valuestring : "";
    const char *action_name = cJSON_IsString(action_item) ? action_item->valuestring : "";
    const int channel_id = cJSON_IsNumber(ch_item) ? ch_item->valueint : -1;

    if (!action_name || !action_name[0])
    {
        send_all(client_fd, make_response(false, req_id, channel_id, "", "", "empty action"));
        cJSON_Delete(root);
        return;
    }

    if (strcmp(scope, "global") == 0)
    {
        const std::string global_instance_id = instance_id ? instance_id : "";
        const std::string logic_name = current_global_logic_name(global_instance_id);
        if (global_instance_id.empty() || logic_name.empty())
        {
            send_all(client_fd, make_response(false, req_id, -1, action_name, "", "unknown global instance_id"));
            cJSON_Delete(root);
            return;
        }
        if (!global_logic_action_get(logic_name.c_str()))
        {
            send_all(client_fd, make_response(false, req_id, -1, action_name, logic_name.c_str(),
                                              "current global logic has no action handler"));
            cJSON_Delete(root);
            return;
        }

        char *payload_text = payload_item ? cJSON_PrintUnformatted(payload_item) : nullptr;
        LogicAction action;
        action.request_id = req_id ? req_id : "";
        action.name = action_name;
        action.payload_json = payload_text ? payload_text : "{}";
        action.logic_name = logic_name;
        action.received_unix_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (payload_text)
            cJSON_free(payload_text);

        enqueue_global_action(action, global_instance_id);
        send_all(client_fd, make_response(true, req_id, -1, action_name, logic_name.c_str(), "accepted"));
        cJSON_Delete(root);
        return;
    }

    if (!app_ctrl_has_channel(channel_id))
    {
        send_all(client_fd, make_response(false, req_id, channel_id, action_name, "", "unknown channel_id"));
        cJSON_Delete(root);
        return;
    }
    /* ---- 系统级动作: 不归属任何特定 logic, 直接处理 ---- */
    if (strcmp(action_name, "infer_toggle") == 0)
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
        int &flag = g_pCtrl->channels_state[channel_id].infer_runtime_enable;
        flag = flag ? 0 : 1;
        const int new_state = flag;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);

        const char *msg = new_state ? "inference enabled" : "inference disabled";
        send_all(client_fd, make_response(true, req_id, channel_id, action_name, "", msg));
        cJSON_Delete(root);
        return;
    }

    const std::string logic_name = current_logic_name(channel_id);
    if (logic_name.empty())
    {
        send_all(client_fd, make_response(false, req_id, channel_id, action_name, "", "logic is empty"));
        cJSON_Delete(root);
        return;
    }

    if (!channel_logic_action_get(logic_name.c_str()))
    {
        send_all(client_fd, make_response(false, req_id, channel_id, action_name, logic_name.c_str(),
                                          "current logic has no action handler"));
        cJSON_Delete(root);
        return;
    }

    char *payload_text = payload_item ? cJSON_PrintUnformatted(payload_item) : nullptr;
    LogicAction action;
    action.request_id = req_id ? req_id : "";
    action.name = action_name ? action_name : "";
    action.payload_json = payload_text ? payload_text : "{}";
    action.logic_name = logic_name;
    action.received_unix_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (payload_text)
        cJSON_free(payload_text);

    enqueue_action(action, channel_id);
    send_all(client_fd, make_response(true, req_id, channel_id, action_name, logic_name.c_str(), "accepted"));
    cJSON_Delete(root);
}

void server_loop()
{
    while (g_running.load())
    {
        struct pollfd pfd{};
        pfd.fd = g_server_fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 200);
        if (!g_running.load())
            break;
        if (pr <= 0 || !(pfd.revents & POLLIN))
            continue;

        const int client_fd = accept(g_server_fd, nullptr, nullptr);
        if (client_fd < 0)
            continue;

        struct timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_client(client_fd);
        close(client_fd);
    }
}

} // namespace

int logic_control_init(void)
{
    if (g_running.load())
        return 0;

    const char *env_path = std::getenv("RK_LOGIC_CONTROL_SOCKET");
    g_socket_path = (env_path && env_path[0]) ? env_path : build_default_socket_path();
    unlink(g_socket_path.c_str());

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0)
    {
        std::fprintf(stderr, "[LogicControl] socket() failed: %s\n", std::strerror(errno));
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (g_socket_path.size() >= sizeof(addr.sun_path))
    {
        std::fprintf(stderr, "[LogicControl] socket path too long: %s\n", g_socket_path.c_str());
        close_fd(g_server_fd);
        return -2;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_socket_path.c_str());

    if (bind(g_server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        std::fprintf(stderr, "[LogicControl] bind(%s) failed: %s\n", g_socket_path.c_str(), std::strerror(errno));
        close_fd(g_server_fd);
        return -3;
    }
    chmod(g_socket_path.c_str(), 0660);

    if (listen(g_server_fd, 8) != 0)
    {
        std::fprintf(stderr, "[LogicControl] listen() failed: %s\n", std::strerror(errno));
        unlink(g_socket_path.c_str());
        close_fd(g_server_fd);
        return -4;
    }

    g_running.store(true);
    g_server_thread = std::thread(server_loop);
    std::printf("[LogicControl] listening on %s\n", g_socket_path.c_str());
    return 0;
}

void logic_control_deinit(void)
{
    g_running.store(false);
    if (g_server_thread.joinable())
        g_server_thread.join();
    close_fd(g_server_fd);
    if (!g_socket_path.empty())
        unlink(g_socket_path.c_str());

    for (int i = 0; i < kMaxChannels; ++i)
    {
        std::lock_guard<std::mutex> lock(g_action_mutex[i]);
        g_action_queue[i].clear();
    }
    std::lock_guard<std::mutex> global_lock(g_global_action_mutex);
    g_global_action_queues.clear();
}

void logic_control_take_channel(int channel_id, std::vector<LogicAction> &out)
{
    out.clear();
    if (channel_id < 0 || channel_id >= kMaxChannels)
        return;

    std::lock_guard<std::mutex> lock(g_action_mutex[channel_id]);
    auto &queue = g_action_queue[channel_id];
    out.reserve(queue.size());
    while (!queue.empty())
    {
        out.push_back(std::move(queue.front()));
        queue.pop_front();
    }
}

void logic_control_take_global(const std::string &instance_id, std::vector<LogicAction> &out)
{
    out.clear();
    if (instance_id.empty())
        return;

    std::lock_guard<std::mutex> lock(g_global_action_mutex);
    auto found = g_global_action_queues.find(instance_id);
    if (found == g_global_action_queues.end())
        return;
    auto &queue = found->second;
    out.reserve(queue.size());
    while (!queue.empty())
    {
        out.push_back(std::move(queue.front()));
        queue.pop_front();
    }
    g_global_action_queues.erase(found);
}

void logic_control_set_channel_inference(int channel_id, bool enable)
{
    if (!app_ctrl_has_channel(channel_id))
        return;

    pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
    const int old_val = g_pCtrl->channels_state[channel_id].infer_runtime_enable;
    g_pCtrl->channels_state[channel_id].infer_runtime_enable = enable ? 1 : 0;
    const int new_val = g_pCtrl->channels_state[channel_id].infer_runtime_enable;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);

    if (old_val != new_val)
        printf("[LogicControl] ch%02d infer %s\n", channel_id, enable ? "ON" : "OFF");
}

int logic_control_get_channel_inference(int channel_id)
{
    if (!app_ctrl_has_channel(channel_id))
        return 0;

    pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
    const int val = g_pCtrl->channels_state[channel_id].infer_runtime_enable;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);
    return val;
}
