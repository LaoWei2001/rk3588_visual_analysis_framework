#include "file_playback_control.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "capturer/decChannel.h"
#include "runtime/app_ctrl.h"
#include <json.h>

namespace
{
constexpr int kMaxRequestBytes = 16 * 1024;
std::atomic<bool> g_running{false};
std::thread g_server_thread;
int g_server_fd = -1;
std::string g_socket_path;

class CapturerGuard
{
  public:
    CapturerGuard()
    {
        if (g_pCtrl)
        {
            pthread_mutex_lock(&g_pCtrl->capturer_mtx);
            locked_ = true;
        }
    }
    ~CapturerGuard()
    {
        if (locked_)
            pthread_mutex_unlock(&g_pCtrl->capturer_mtx);
    }

  private:
    bool locked_ = false;
};

std::string default_socket_path()
{
    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/rk3588_file_playback_%d.sock", static_cast<int>(getpid()));
    return path;
}

void close_fd(int &fd)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

void send_json(int fd, cJSON *root)
{
    char *encoded = root ? cJSON_PrintUnformatted(root) : nullptr;
    std::string text = encoded ? encoded : "{\"ok\":false,\"message\":\"encode failed\"}";
    if (encoded)
        cJSON_free(encoded);
    text.push_back('\n');
    const char *cursor = text.data();
    size_t remaining = text.size();
    while (remaining > 0)
    {
        const ssize_t count = send(fd, cursor, remaining, 0);
        if (count <= 0)
            break;
        cursor += count;
        remaining -= static_cast<size_t>(count);
    }
}

cJSON *error_response(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON_AddStringToObject(root, "message", message ? message : "unknown error");
    return root;
}

cJSON *status_response()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON *sources = cJSON_AddArrayToObject(root, "sources");
    if (!g_pCtrl)
        return root;

    CapturerGuard guard;
    for (int owner = 0; owner < APP_CTRL_MAX_CAPTURERS; ++owner)
    {
        DecChannel *capturer = g_pCtrl->capturers[owner];
        if (!capturer || !capturer->isFileSource())
            continue;

        FilePlaybackStatus status;
        capturer->queryFilePlayback(status);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "owner_channel_id", capturer->channelId());
        cJSON *channels = cJSON_AddArrayToObject(item, "channel_ids");
        for (int channel_id : capturer->channelIds())
            cJSON_AddItemToArray(channels, cJSON_CreateNumber(channel_id));
        cJSON_AddStringToObject(item, "location", capturer->sourceLocation().c_str());
        cJSON_AddBoolToObject(item, "available", status.available ? 1 : 0);
        cJSON_AddBoolToObject(item, "seekable", status.seekable ? 1 : 0);
        cJSON_AddBoolToObject(item, "playing", status.playing ? 1 : 0);
        cJSON_AddBoolToObject(item, "ended", capturer->isFileEos() ? 1 : 0);
        cJSON_AddNumberToObject(item, "position_ms", static_cast<double>(status.position_ms));
        cJSON_AddNumberToObject(item, "duration_ms", static_cast<double>(status.duration_ms));
        cJSON_AddItemToArray(sources, item);
    }
    return root;
}

cJSON *seek_response(int channel_id, int64_t position_ms)
{
    if (!g_pCtrl)
        return error_response("runtime is not ready");
    CapturerGuard guard;
    DecChannel *target = nullptr;
    for (int owner = 0; owner < APP_CTRL_MAX_CAPTURERS; ++owner)
    {
        DecChannel *capturer = g_pCtrl->capturers[owner];
        if (capturer && capturer->isFileSource() && capturer->hasChannel(channel_id))
        {
            target = capturer;
            break;
        }
    }
    if (!target)
        return error_response("local file source not found for channel");

    int64_t actual_ms = 0;
    std::string error;
    if (!target->seekFilePlayback(position_ms, actual_ms, error))
        return error_response(error.c_str());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddNumberToObject(root, "channel_id", channel_id);
    cJSON_AddNumberToObject(root, "position_ms", static_cast<double>(actual_ms));
    cJSON_AddStringToObject(root, "message", "seek completed");
    return root;
}

void handle_client(int client_fd)
{
    char buffer[kMaxRequestBytes + 1];
    const ssize_t count = recv(client_fd, buffer, kMaxRequestBytes, 0);
    if (count <= 0)
        return;
    buffer[count] = '\0';
    cJSON *request = cJSON_Parse(buffer);
    if (!request)
    {
        cJSON *response = error_response("invalid json");
        send_json(client_fd, response);
        cJSON_Delete(response);
        return;
    }

    const cJSON *command_item = cJSON_GetObjectItemCaseSensitive(request, "command");
    const char *command = cJSON_IsString(command_item) ? command_item->valuestring : "";
    cJSON *response = nullptr;
    if (std::strcmp(command, "status") == 0)
    {
        response = status_response();
    }
    else if (std::strcmp(command, "seek") == 0)
    {
        const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(request, "channel_id");
        const cJSON *position_item = cJSON_GetObjectItemCaseSensitive(request, "position_ms");
        if (!cJSON_IsNumber(channel_item) || !cJSON_IsNumber(position_item) || position_item->valuedouble < 0)
            response = error_response("seek requires channel_id and non-negative position_ms");
        else
            response = seek_response(channel_item->valueint, static_cast<int64_t>(position_item->valuedouble));
    }
    else
    {
        response = error_response("unknown command");
    }
    send_json(client_fd, response);
    cJSON_Delete(response);
    cJSON_Delete(request);
}

void server_loop()
{
    while (g_running.load())
    {
        pollfd descriptor{};
        descriptor.fd = g_server_fd;
        descriptor.events = POLLIN;
        const int result = poll(&descriptor, 1, 200);
        if (!g_running.load())
            break;
        if (result <= 0 || !(descriptor.revents & POLLIN))
            continue;
        const int client_fd = accept(g_server_fd, nullptr, nullptr);
        if (client_fd < 0)
            continue;
        timeval timeout{};
        timeout.tv_sec = 3;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_client(client_fd);
        close(client_fd);
    }
}
} // namespace

int file_playback_control_init(void)
{
    if (g_running.load())
        return 0;
    const char *configured = std::getenv("RK_FILE_PLAYBACK_CONTROL_SOCKET");
    g_socket_path = configured && configured[0] ? configured : default_socket_path();
    unlink(g_socket_path.c_str());

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (g_socket_path.size() >= sizeof(address.sun_path))
    {
        close_fd(g_server_fd);
        return -2;
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", g_socket_path.c_str());
    if (bind(g_server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        std::fprintf(stderr, "[FilePlayback] bind(%s) failed: %s\n", g_socket_path.c_str(), std::strerror(errno));
        close_fd(g_server_fd);
        return -3;
    }
    chmod(g_socket_path.c_str(), 0660);
    if (listen(g_server_fd, 8) != 0)
    {
        unlink(g_socket_path.c_str());
        close_fd(g_server_fd);
        return -4;
    }
    g_running.store(true);
    g_server_thread = std::thread(server_loop);
    std::printf("[FilePlayback] listening on %s\n", g_socket_path.c_str());
    return 0;
}

void file_playback_control_deinit(void)
{
    g_running.store(false);
    if (g_server_thread.joinable())
        g_server_thread.join();
    close_fd(g_server_fd);
    if (!g_socket_path.empty())
        unlink(g_socket_path.c_str());
}
