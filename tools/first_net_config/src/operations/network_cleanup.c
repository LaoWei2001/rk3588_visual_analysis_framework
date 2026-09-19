#define _POSIX_C_SOURCE 200809L

#include "operations/network_cleanup.h"

#include "common/cli_io.h"
#include "common/command_runner.h"
#include "inspection/interface_inspector.h"
#include "common/netconfig_types.h"
#include "inspection/network_state.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_STATE_DIR "/var/lib/first_net_config"
#define DEFAULT_NM_PARENT "/etc/NetworkManager"
#define RUNTIME_NM_PARENT "/run/NetworkManager"
#define NM_CONNECTION_DIR "system-connections"
#define MAX_CLEAR_CONNECTIONS 512
#define UUID_LIST_OUTPUT_SIZE 32768

typedef struct
{
    char uuid[UUID_SIZE];
    char device[64];
    bool active;
    bool ssh_path;
} ClearConnection;

static const char *state_dir(void)
{
    const char *override = getenv("FIRST_NET_CONFIG_STATE_DIR");
    return override && override[0] ? override : DEFAULT_STATE_DIR;
}

static const char *nm_parent(void)
{
    const char *override = getenv("FIRST_NET_CONFIG_NM_PARENT");
    return override && override[0] ? override : DEFAULT_NM_PARENT;
}

static const char *runtime_nm_parent(void)
{
    const char *override = getenv("FIRST_NET_CONFIG_RUNTIME_NM_PARENT");
    return override && override[0] ? override : RUNTIME_NM_PARENT;
}

static bool join_path(char *out, size_t size,
                      const char *left, const char *right)
{
    int written = snprintf(out, size, "%s/%s", left, right);
    return written >= 0 && (size_t)written < size;
}

static bool ensure_directory(const char *path, mode_t mode)
{
    struct stat info;

    if (mkdir(path, mode) != 0 && errno != EEXIST)
    {
        return false;
    }
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid())
    {
        errno = EPERM;
        return false;
    }
    return chmod(path, mode) == 0;
}

static bool valid_uuid_text(const char *uuid)
{
    if (!uuid || strlen(uuid) != UUID_SIZE - 1)
    {
        return false;
    }
    for (size_t index = 0; index < UUID_SIZE - 1; ++index)
    {
        bool dash = index == 8 || index == 13 ||
                    index == 18 || index == 23;
        bool hex = (uuid[index] >= '0' && uuid[index] <= '9') ||
                   (uuid[index] >= 'a' && uuid[index] <= 'f') ||
                   (uuid[index] >= 'A' && uuid[index] <= 'F');

        if ((dash && uuid[index] != '-') || (!dash && !hex))
        {
            return false;
        }
    }
    return true;
}

static int find_uuid(const ClearConnection *connections, int count,
                     const char *uuid)
{
    for (int index = 0; index < count; ++index)
    {
        if (strcmp(connections[index].uuid, uuid) == 0)
        {
            return index;
        }
    }
    return -1;
}

static int collect_connections(ClearConnection *connections, int capacity)
{
    char output[UUID_LIST_OUTPUT_SIZE];
    const char *all_args[] = {
        "nmcli", "-t", "-f", "UUID,DEVICE",
        "connection", "show", NULL};
    const char *active_args[] = {
        "nmcli", "-t", "-f", "UUID,DEVICE",
        "connection", "show", "--active", NULL};
    int count = 0;

    if (!connections || capacity <= 0 ||
        capture_cmd(all_args, output, sizeof(output)) != 0)
    {
        return -1;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\n", &saveptr);

        while (line)
        {
            size_t length = strlen(line);
            char uuid[UUID_SIZE];

            if (length < UUID_SIZE || line[UUID_SIZE - 1] != ':' ||
                count >= capacity)
            {
                printf("[拒绝] 保存连接清单异常或数量过多。\n");
                return -1;
            }
            memcpy(uuid, line, UUID_SIZE - 1);
            uuid[UUID_SIZE - 1] = '\0';
            if (!valid_uuid_text(uuid))
            {
                printf("[拒绝] 保存连接的内部标识无效。\n");
                return -1;
            }
            if (find_uuid(connections, count, uuid) < 0)
            {
                snprintf(connections[count].uuid,
                         sizeof(connections[count].uuid), "%s", uuid);
                snprintf(connections[count].device,
                         sizeof(connections[count].device), "%s",
                         line + UUID_SIZE);
                ++count;
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    if (capture_cmd(active_args, output, sizeof(output)) != 0)
    {
        return -1;
    }
    {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\n", &saveptr);

        while (line)
        {
            if (strlen(line) >= UUID_SIZE && line[UUID_SIZE - 1] == ':')
            {
                char uuid[UUID_SIZE];
                int index;

                memcpy(uuid, line, UUID_SIZE - 1);
                uuid[UUID_SIZE - 1] = '\0';
                index = find_uuid(connections, count, uuid);
                if (index >= 0)
                {
                    connections[index].active = true;
                    if (connections[index].device[0] == '\0')
                    {
                        snprintf(connections[index].device,
                                 sizeof(connections[index].device), "%s",
                                 line + UUID_SIZE);
                    }
                    connections[index].ssh_path =
                        connections[index].device[0] != '\0' &&
                        interface_is_ssh_path(connections[index].device);
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    return count;
}

static int count_profile_files(const char *directory, bool absent_is_empty)
{
    DIR *handle = opendir(directory);
    struct dirent *entry;
    int count = 0;
    int directory_fd;

    if (!handle)
    {
        return absent_is_empty && errno == ENOENT ? 0 : -1;
    }
    directory_fd = dirfd(handle);
    while ((entry = readdir(handle)) != NULL)
    {
        struct stat info;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        if (fstatat(directory_fd, entry->d_name, &info,
                    AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(info.st_mode))
        {
            closedir(handle);
            return -1;
        }
        ++count;
    }
    if (closedir(handle) != 0)
    {
        return -1;
    }
    return count;
}

static bool clear_profile_files(const char *directory, bool absent_is_ok)
{
    DIR *handle;
    struct dirent *entry;
    int directory_fd;

    if (count_profile_files(directory, absent_is_ok) < 0)
    {
        return false;
    }
    handle = opendir(directory);
    if (!handle)
    {
        return absent_is_ok && errno == ENOENT;
    }
    directory_fd = dirfd(handle);
    while ((entry = readdir(handle)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        if (unlinkat(directory_fd, entry->d_name, 0) != 0)
        {
            closedir(handle);
            return false;
        }
    }
    return closedir(handle) == 0;
}

static bool create_emergency_backup(char *out, size_t out_size)
{
    char backups[PATH_MAX];
    char connections[PATH_MAX];
    char stamp[32];
    time_t now = time(NULL);
    struct tm local;
    const char *tar_args[9];
    int written;

    if (!join_path(backups, sizeof(backups), state_dir(), "backups") ||
        !ensure_directory(state_dir(), 0700) ||
        !ensure_directory(backups, 0700) ||
        !join_path(connections, sizeof(connections),
                   nm_parent(), NM_CONNECTION_DIR))
    {
        return false;
    }
    if (count_profile_files(connections, false) < 0)
    {
        return false;
    }
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    written = snprintf(out, out_size,
                       "%s/before-clear-all-%s-%ld.tar",
                       backups, stamp, (long)getpid());
    if (written < 0 || (size_t)written >= out_size)
    {
        return false;
    }

    tar_args[0] = "tar";
    tar_args[1] = "--create";
    tar_args[2] = "--file";
    tar_args[3] = out;
    tar_args[4] = "--directory";
    tar_args[5] = nm_parent();
    tar_args[6] = NM_CONNECTION_DIR;
    tar_args[7] = NULL;
    if (run_cmd(tar_args) != 0 || chmod(out, 0600) != 0)
    {
        (void)unlink(out);
        return false;
    }
    return true;
}

static void clear_persistent_after_reboot(void)
{
    char directory[PATH_MAX];
    char backup[PATH_MAX];

    printf("\n[永久操作] 将删除全部永久保存的连接。\n");
    printf("当前连接不会主动断开，但 SSH 仍有中断风险。\n");
    printf("临时连接和剩余内存状态会在设备重启后消失。\n");
    terminal_ui_set_step("确认重启后清空",
                         "删除永久连接文件，当前连接暂不主动断开");
    if (!read_exact_word(
            "确认重启后清空请输入 CLEAR PERSISTENT NETWORKS: ",
            "CLEAR PERSISTENT NETWORKS"))
    {
        printf("已取消。\n");
        return;
    }
    if (!create_emergency_backup(backup, sizeof(backup)))
    {
        printf("[拒绝] 无法创建紧急备份，未执行清除。\n");
        return;
    }
    if (!join_path(directory, sizeof(directory),
                   nm_parent(), NM_CONNECTION_DIR) ||
        !clear_profile_files(directory, false))
    {
        printf("[失败] 清除过程中发生错误。不要重启；紧急备份：%s\n",
               backup);
        return;
    }
    printf("\n[完成] 永久连接文件已清空，程序未主动断开当前网络。\n");
    printf("重启后临时/内存连接也会消失。备份：%s\n", backup);
}

static void clear_immediately(ClearConnection *connections, int count)
{
    char persistent[PATH_MAX];
    char runtime[PATH_MAX];
    char backup[PATH_MAX];
    int failures = 0;
    int deleted = 0;

    printf("\n[极高风险] 将立即删除全部连接，有线/Wi-Fi 可能全部中断。\n");
    printf("SSH 一定会断开（其连接最后删除）。\n");
    terminal_ui_set_step("确认立即清空",
                         "所有连接将立即删除，SSH 会断开");
    if (!read_exact_word("确认立即清空请输入 DELETE ALL NETWORKS: ",
                         "DELETE ALL NETWORKS"))
    {
        printf("已取消。\n");
        return;
    }
    if (!create_emergency_backup(backup, sizeof(backup)))
    {
        printf("[拒绝] 无法创建紧急备份，未执行清除。\n");
        return;
    }
    printf("紧急备份已创建：%s\n", backup);

    /* SSH 断开通常会向会话进程发送 SIGHUP；忽略它以完成剩余清除。 */
    if (signal(SIGHUP, SIG_IGN) == SIG_ERR ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        printf("[拒绝] 无法启用 SSH 断线保护，未执行清除。\n");
        return;
    }

    for (int phase = 0; phase < 3; ++phase)
    {
        for (int index = 0; index < count; ++index)
        {
            bool selected =
                (phase == 0 && !connections[index].active) ||
                (phase == 1 && connections[index].active &&
                 !connections[index].ssh_path) ||
                (phase == 2 && connections[index].active &&
                 connections[index].ssh_path);

            if (!selected)
            {
                continue;
            }
            if (delete_connection_by_uuid(connections[index].uuid) == 0)
            {
                ++deleted;
            }
            else
            {
                ++failures;
            }
        }
    }

    if (!join_path(persistent, sizeof(persistent),
                   nm_parent(), NM_CONNECTION_DIR) ||
        !join_path(runtime, sizeof(runtime),
                   runtime_nm_parent(), NM_CONNECTION_DIR) ||
        !clear_profile_files(persistent, false) ||
        !clear_profile_files(runtime, true))
    {
        ++failures;
    }

    printf("\n删除结果：成功删除 %d 个连接，失败项 %d。\n",
           deleted, failures);
    if (failures == 0)
    {
        printf("[完成] 保存连接和残留配置已清空。\n");
    }
    else
    {
        printf("[警告] 清除不完整。请通过本地串口/显示器重新运行并检查。\n");
    }
    printf("紧急备份：%s\n", backup);
}

void clear_all_network_connections(void)
{
    ClearConnection connections[MAX_CLEAR_CONNECTIONS] = {0};
    int count;
    int choice;

    printf("\n========== 清除全部保存连接 ==========\n");
    printf("本功能不依赖初始网络基准，也不会删除系统驱动、网络服务、\n");
    printf("系统级策略、初始基准或 Web Console 摄像头发现信息。\n");
    count = collect_connections(connections, MAX_CLEAR_CONNECTIONS);
    if (count < 0)
    {
        printf("[拒绝] 无法可靠读取完整连接清单。\n");
        return;
    }
    {
        int active_count = 0;
        int ssh_count = 0;

        for (int index = 0; index < count; ++index)
        {
            active_count += connections[index].active ? 1 : 0;
            ssh_count += connections[index].ssh_path ? 1 : 0;
        }
        printf("\n保存连接：%d 项；已启用：%d 项%s\n",
               count, active_count,
               ssh_count > 0 ? "；包含当前 SSH 连接" : "");
    }
clear_choice:
    terminal_ui_set_step("选择清除方式",
                         "可选择重启后失效或立即清除");
    printf("请选择清除方式：\n");
    printf("  1. 只清除永久连接文件，重启后全部失效（较安全）\n");
    printf("  2. 立即清除全部连接，包括已启用连接（SSH 会断开）\n");
    printf("  0. 取消\n");
    choice = read_int("请选择 [0-2]: ", 0, 2);

    if (choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        return;
    }

    if (choice == 1)
    {
        terminal_ui_prepare_step(true);
        clear_persistent_after_reboot();
        if (terminal_ui_back_requested())
        {
            goto clear_choice;
        }
    }
    else if (choice == 2)
    {
        terminal_ui_prepare_step(true);
        clear_immediately(connections, count);
        if (terminal_ui_back_requested())
        {
            goto clear_choice;
        }
    }
    else
    {
        printf("已取消。\n");
    }
}
