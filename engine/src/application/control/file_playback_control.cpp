#include "file_playback_control.h"
#include "socket_shutdown.h"

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

#include "application/runtime/app_ctrl.h"
#include "common/pthread_lock.h"
#include <cmath>
#include <json.h>
#include <limits>

namespace
{
constexpr int kMaxRequestBytes = 16 * 1024;
std::atomic<bool> g_running{false};
std::thread g_server_thread;
int g_server_fd = -1;
ControlConnection g_connection;
std::string g_socket_path;

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

    for (const auto &status : file_playback_status())
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "owner_channel_id", status.channel_id);
        cJSON *channels = cJSON_AddArrayToObject(item, "channel_ids");
        cJSON_AddItemToArray(channels, cJSON_CreateNumber(status.channel_id));
        cJSON_AddStringToObject(item, "location", status.location.c_str());
        cJSON_AddBoolToObject(item, "available", status.available);
        cJSON_AddBoolToObject(item, "seekable", status.seekable);
        cJSON_AddBoolToObject(item, "playing", status.playing);
        cJSON_AddBoolToObject(item, "paused", status.paused);
        cJSON_AddBoolToObject(item, "pause_requested", status.pause_requested);
        cJSON_AddBoolToObject(item, "busy", status.busy);
        cJSON_AddBoolToObject(item, "ended", status.ended);
        cJSON_AddNumberToObject(item, "position_ms", static_cast<double>(status.position_ms));
        cJSON_AddNumberToObject(item, "duration_ms", static_cast<double>(status.duration_ms));
        cJSON_AddStringToObject(item, "error", status.error.c_str());
        cJSON_AddItemToArray(sources, item);
    }
    return root;
}

cJSON *command_response(int channel_id, int64_t position_ms, const bool *paused)
{
    std::string error;
    const bool accepted = paused ? file_playback_set_paused(channel_id, *paused, &error)
                                 : file_playback_seek(channel_id, position_ms, &error);
    if (!accepted)
        return error_response(error.c_str());
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "channel_id", channel_id);
    cJSON_AddBoolToObject(root, "busy", true);
    if (paused)
        cJSON_AddBoolToObject(root, "pause_requested", *paused);
    else
        cJSON_AddNumberToObject(root, "position_ms", static_cast<double>(position_ms));
    return root;
}

bool valid_channel(const cJSON *item)
{
    return cJSON_IsNumber(item) && std::isfinite(item->valuedouble) && item->valuedouble >= 0 &&
           item->valuedouble < MAX_CHANNEL_NUM && std::floor(item->valuedouble) == item->valuedouble;
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
        if (!valid_channel(channel_item) || !cJSON_IsNumber(position_item) ||
            !std::isfinite(position_item->valuedouble) || position_item->valuedouble < 0 ||
            position_item->valuedouble > 9007199254740991.0 ||
            std::floor(position_item->valuedouble) != position_item->valuedouble)
            response = error_response("seek requires channel_id and non-negative position_ms");
        else
            response =
                command_response(channel_item->valueint, static_cast<int64_t>(position_item->valuedouble), nullptr);
    }
    else if (std::strcmp(command, "pause") == 0)
    {
        const auto *channel = cJSON_GetObjectItemCaseSensitive(request, "channel_id");
        const auto *value = cJSON_GetObjectItemCaseSensitive(request, "paused");
        if (!valid_channel(channel) || !cJSON_IsBool(value))
            response = error_response("pause requires channel_id and boolean paused");
        else
        {
            const bool paused = cJSON_IsTrue(value);
            response = command_response(channel->valueint, 0, &paused);
        }
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
        if (!g_connection.begin(client_fd, g_running))
            break;
        timeval timeout{};
        timeout.tv_sec = 3;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_client(client_fd);
        g_connection.finish();
    }
}
} // namespace

std::vector<vision::FilePlaybackStatus> file_playback_status()
{
    std::vector<vision::FilePlaybackStatus> statuses;
    const auto runtime = app_ctrl_get_runtime_snapshot();
    if (!g_pCtrl || !runtime)
        return statuses;
    for (const auto &channel : runtime->config.channels)
    {
        if (!channel.enable || channel.stream.src_type != "file")
            continue;
        PthreadLock lock(g_pCtrl->chn_mtx[channel.id]);
        auto status = g_pCtrl->channels_state[channel.id].file_playback;
        status.channel_id = channel.id;
        status.location = channel.stream.url;
        statuses.push_back(std::move(status));
    }
    return statuses;
}

static bool request_file_control(int channel_id, const bool *paused, int64_t position, std::string *error)
{
    if (error)
        error->clear();
    const auto fail = [&](const char *message) {
        if (error)
            *error = message;
        return false;
    };
    if (!g_pCtrl || !g_pCtrl->isRunning || !app_ctrl_has_channel(channel_id))
        return fail("视频通道未运行");
    const auto runtime = app_ctrl_get_runtime_snapshot();
    const auto *channel = app_ctrl_runtime_channel_config(runtime, channel_id);
    if (!channel || channel->stream.src_type != "file")
        return fail("只有视频文件支持暂停和跳转");
    std::function<void()> wakeup;
    {
        PthreadLock lock(g_pCtrl->chn_mtx[channel_id]);
        auto &state = g_pCtrl->channels_state[channel_id];
        auto &status = state.file_playback;
        if (!status.available)
            return fail("文件播放尚未就绪");
        if (paused)
        {
            if (status.ended)
                return fail("文件已播放结束，请先跳转到需要的位置");
            status.pause_requested = *paused;
        }
        else
        {
            if (position < 0 || position > INT64_MAX / 1000000 || !status.seekable)
                return fail("文件不支持跳转或位置无效");
            state.playback_seek_ms = position;
        }
        ++state.playback_request;
        status.busy = true;
        status.error.clear();
        wakeup = state.playback_wakeup;
    }
    if (wakeup)
        wakeup();
    publication_signal_notify();
    return true;
}

bool file_playback_set_paused(int channel_id, bool paused, std::string *error)
{
    return request_file_control(channel_id, &paused, 0, error);
}

bool file_playback_seek(int channel_id, int64_t position_ms, std::string *error)
{
    return request_file_control(channel_id, nullptr, position_ms, error);
}

int file_playback_control_init(void)
{
    if (g_running.load())
        return 0;
    const char *configured = std::getenv("RK_FILE_PLAYBACK_CONTROL_SOCKET");
    g_socket_path = configured && configured[0] ? configured : default_socket_path();
    unlink(g_socket_path.c_str());

    g_server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
    g_connection.cancel();
    if (g_server_fd >= 0)
        shutdown(g_server_fd, SHUT_RDWR);
    if (g_server_thread.joinable())
        g_server_thread.join();
    close_fd(g_server_fd);
    if (!g_socket_path.empty())
        unlink(g_socket_path.c_str());
}
