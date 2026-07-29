#include "channel_control.h"

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
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../core/app_ctrl.h"
#include "../third_party/json/cJSON.h"

namespace
{

constexpr int kMaxChannels = APP_CTRL_MAX_CAPTURERS;
constexpr int kMaxQueuePerChannel = 64;
constexpr int kMaxRequestBytes = 64 * 1024;

std::mutex g_action_mutex[kMaxChannels];
std::deque<ChannelAction> g_action_queue[kMaxChannels];
std::atomic<bool> g_running{false};
std::thread g_server_thread;
int g_server_fd = -1;
std::string g_socket_path;

std::string build_default_socket_path()
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/rk3588_channel_control_%d.sock", static_cast<int>(getpid()));
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

void enqueue_action(const ChannelAction &action, int channel_id)
{
    std::lock_guard<std::mutex> lock(g_action_mutex[channel_id]);
    auto &queue = g_action_queue[channel_id];
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
    const cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(root, "channel_id");
    const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(root, "payload");

    const char *req_id = cJSON_IsString(req_id_item) ? req_id_item->valuestring : "";
    const char *action_name = cJSON_IsString(action_item) ? action_item->valuestring : "";
    const int channel_id = cJSON_IsNumber(ch_item) ? ch_item->valueint : -1;

    if (!app_ctrl_has_channel(channel_id))
    {
        send_all(client_fd, make_response(false, req_id, channel_id, action_name, "", "unknown channel_id"));
        cJSON_Delete(root);
        return;
    }
    if (!action_name || !action_name[0])
    {
        send_all(client_fd, make_response(false, req_id, channel_id, "", "", "empty action"));
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
    ChannelAction action;
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
        struct pollfd pfd
        {
        };
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

        struct timeval tv
        {
        };
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_client(client_fd);
        close(client_fd);
    }
}

} // namespace

int channel_control_init(void)
{
    if (g_running.load())
        return 0;

    const char *env_path = std::getenv("RK_CHANNEL_CONTROL_SOCKET");
    g_socket_path = (env_path && env_path[0]) ? env_path : build_default_socket_path();
    unlink(g_socket_path.c_str());

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0)
    {
        std::fprintf(stderr, "[ChannelControl] socket() failed: %s\n", std::strerror(errno));
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (g_socket_path.size() >= sizeof(addr.sun_path))
    {
        std::fprintf(stderr, "[ChannelControl] socket path too long: %s\n", g_socket_path.c_str());
        close_fd(g_server_fd);
        return -2;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_socket_path.c_str());

    if (bind(g_server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        std::fprintf(stderr, "[ChannelControl] bind(%s) failed: %s\n", g_socket_path.c_str(), std::strerror(errno));
        close_fd(g_server_fd);
        return -3;
    }
    chmod(g_socket_path.c_str(), 0660);

    if (listen(g_server_fd, 8) != 0)
    {
        std::fprintf(stderr, "[ChannelControl] listen() failed: %s\n", std::strerror(errno));
        unlink(g_socket_path.c_str());
        close_fd(g_server_fd);
        return -4;
    }

    g_running.store(true);
    g_server_thread = std::thread(server_loop);
    std::printf("[ChannelControl] listening on %s\n", g_socket_path.c_str());
    return 0;
}

void channel_control_deinit(void)
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
}

void channel_control_take(int channel_id, std::vector<ChannelAction> &out)
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
