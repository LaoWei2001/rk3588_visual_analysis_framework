#define _POSIX_C_SOURCE 200809L

#include "factory_restore.h"
#include "cli_io.h"
#include "command_runner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#define NM_CONNECTION_DIR "system-connections"

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

static const char *apps_root(void)
{
    const char *override = getenv("APPS_ROOT");
    return override && override[0] ? override : "/opt/ai_apps";
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

static bool baseline_archive(char *out, size_t size)
{
    return join_path(out, size, state_dir(), "factory-system-connections.tar");
}

static bool baseline_machine_id(char *out, size_t size)
{
    return join_path(out, size, state_dir(), "factory-machine-id");
}

static bool baseline_checksum(char *out, size_t size)
{
    return join_path(out, size, state_dir(), "factory-system-connections.sha256");
}

static bool baseline_camera_file(char *out, size_t size)
{
    return join_path(out, size, state_dir(), "factory-camera-network.json");
}

static bool baseline_camera_absent(char *out, size_t size)
{
    return join_path(out, size, state_dir(), "factory-camera-network.absent");
}

static bool current_camera_file(char *out, size_t size)
{
    const char *override = getenv("CAMERA_CONFIG_FILE");
    if (override && override[0])
    {
        int written = snprintf(out, size, "%s", override);
        return written >= 0 && (size_t)written < size;
    }
    return join_path(out, size, apps_root(), ".camera_network.json");
}

static bool camera_transaction_file(char *out, size_t size)
{
    const char *override = getenv("CAMERA_TRANSACTION_FILE");
    if (override && override[0])
    {
        int written = snprintf(out, size, "%s", override);
        return written >= 0 && (size_t)written < size;
    }
    return join_path(out, size, apps_root(),
                     ".camera_network_transaction.json");
}

static bool file_exists_regular(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool copy_text_file(const char *source, const char *destination)
{
    int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int output;
    char buffer[4096];

    if (input < 0)
    {
        return false;
    }
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                               O_NOFOLLOW, 0600);
    if (output < 0)
    {
        close(input);
        return false;
    }
    for (;;)
    {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count == 0)
        {
            break;
        }
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            close(input);
            close(output);
            (void)unlink(destination);
            return false;
        }
        for (ssize_t offset = 0; offset < count;)
        {
            ssize_t written = write(output, buffer + offset,
                                    (size_t)(count - offset));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
            {
                close(input);
                close(output);
                (void)unlink(destination);
                return false;
            }
            offset += written;
        }
    }
    (void)fsync(output);
    close(input);
    return close(output) == 0;
}

static bool write_exclusive_text(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                O_NOFOLLOW, 0600);
    size_t size = text ? strlen(text) : 0;
    size_t used = 0;

    if (descriptor < 0)
    {
        return false;
    }
    while (used < size)
    {
        ssize_t count = write(descriptor, text + used, size - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            close(descriptor);
            (void)unlink(path);
            return false;
        }
        used += (size_t)count;
    }
    (void)fsync(descriptor);
    return close(descriptor) == 0;
}

static bool archive_hash(const char *archive, char *out, size_t out_size)
{
    char result[256];
    const char *argv[] = {"sha256sum", archive, NULL};

    if (capture_cmd(argv, result, sizeof(result)) != 0 || strlen(result) < 64 ||
        out_size < 65)
    {
        return false;
    }
    for (int index = 0; index < 64; ++index)
    {
        char value = result[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F')))
        {
            return false;
        }
        out[index] = value;
    }
    out[64] = '\0';
    return true;
}

static bool checksum_matches(const char *archive)
{
    char checksum_path[PATH_MAX];
    char actual[65];
    char expected[80] = {0};
    FILE *handle;

    if (!baseline_checksum(checksum_path, sizeof(checksum_path)) ||
        !file_exists_regular(checksum_path) ||
        !archive_hash(archive, actual, sizeof(actual)))
    {
        return false;
    }
    handle = fopen(checksum_path, "r");
    if (!handle || !fgets(expected, sizeof(expected), handle))
    {
        if (handle)
            fclose(handle);
        return false;
    }
    fclose(handle);
    trim_space(expected);
    return strcmp(actual, expected) == 0;
}

static bool machine_matches(void)
{
    char saved_path[PATH_MAX];
    char current[128] = {0};
    char saved[128] = {0};
    FILE *handle;

    if (!baseline_machine_id(saved_path, sizeof(saved_path)))
    {
        return false;
    }
    handle = fopen("/etc/machine-id", "r");
    if (!handle || !fgets(current, sizeof(current), handle))
    {
        if (handle)
            fclose(handle);
        return false;
    }
    fclose(handle);
    handle = fopen(saved_path, "r");
    if (!handle || !fgets(saved, sizeof(saved), handle))
    {
        if (handle)
            fclose(handle);
        return false;
    }
    fclose(handle);
    trim_space(current);
    trim_space(saved);
    return current[0] && strcmp(current, saved) == 0;
}

static bool baseline_valid(void)
{
    char archive[PATH_MAX];
    char machine[PATH_MAX];
    char camera[PATH_MAX];
    char camera_absent[PATH_MAX];
    const char *check[5];

    if (!baseline_archive(archive, sizeof(archive)) ||
        !baseline_machine_id(machine, sizeof(machine)) ||
        !baseline_camera_file(camera, sizeof(camera)) ||
        !baseline_camera_absent(camera_absent, sizeof(camera_absent)) ||
        !file_exists_regular(archive) || !file_exists_regular(machine) ||
        (!file_exists_regular(camera) && !file_exists_regular(camera_absent)) ||
        !machine_matches() || !checksum_matches(archive))
    {
        return false;
    }
    check[0] = "tar";
    check[1] = "-tf";
    check[2] = archive;
    check[3] = NULL;
    return run_cmd_silent(check) == 0;
}

void show_factory_baseline_status(void)
{
    printf("\n========== 系统初始网络基准 ==========\n");
    printf("保存目录：%s\n", state_dir());
    if (baseline_valid())
    {
        printf("状态：[可用] 基准属于当前设备，归档可以读取。\n");
    }
    else
    {
        printf("状态：[不可用] 尚未保存可信基准，或基准不属于当前设备。\n");
        printf("程序不会根据当前状态猜测出厂网络。\n");
    }
}

void capture_factory_baseline(void)
{
    char archive[PATH_MAX];
    char machine[PATH_MAX];
    char checksum_path[PATH_MAX];
    char checksum[65];
    char camera_source[PATH_MAX];
    char camera_baseline[PATH_MAX];
    char camera_absent[PATH_MAX];
    const char *tar_args[9];

    show_factory_baseline_status();
    if (baseline_valid())
    {
        printf("[拒绝] 初始基准已经存在。第一版不允许从菜单覆盖它。\n");
        return;
    }
    printf("\n此操作会把当前配置定义为系统初始状态。\n");
    printf("只能在制作系统镜像或确认设备尚未被现场修改时执行。\n");
    if (!read_exact_word("确认当前就是初始状态请输入 CAPTURE BASELINE: ",
                         "CAPTURE BASELINE"))
    {
        printf("已取消。\n");
        return;
    }
    if (!ensure_directory(state_dir(), 0700) ||
        !baseline_archive(archive, sizeof(archive)) ||
        !baseline_machine_id(machine, sizeof(machine)) ||
        !baseline_checksum(checksum_path, sizeof(checksum_path)) ||
        !current_camera_file(camera_source, sizeof(camera_source)) ||
        !baseline_camera_file(camera_baseline, sizeof(camera_baseline)) ||
        !baseline_camera_absent(camera_absent, sizeof(camera_absent)))
    {
        perror("准备基准目录");
        return;
    }

    tar_args[0] = "tar";
    tar_args[1] = "--create";
    tar_args[2] = "--file";
    tar_args[3] = archive;
    tar_args[4] = "--directory";
    tar_args[5] = nm_parent();
    tar_args[6] = NM_CONNECTION_DIR;
    tar_args[7] = NULL;
    if (run_cmd(tar_args) != 0 || chmod(archive, 0600) != 0 ||
        !copy_text_file("/etc/machine-id", machine) ||
        !archive_hash(archive, checksum, sizeof(checksum)) ||
        !write_exclusive_text(checksum_path, checksum) ||
        (file_exists_regular(camera_source)
             ? !copy_text_file(camera_source, camera_baseline)
             : !write_exclusive_text(camera_absent, "absent\n")))
    {
        printf("[失败] 无法保存初始网络基准。\n");
        (void)unlink(archive);
        (void)unlink(machine);
        (void)unlink(checksum_path);
        (void)unlink(camera_baseline);
        (void)unlink(camera_absent);
        return;
    }
    printf("[完成] 已保存 NetworkManager 与摄像头应用配置的只读初始基准。\n");
}

void preview_factory_restore(void)
{
    char archive[PATH_MAX];
    const char *list_archive[4];
    const char *list_current[] = {
        "nmcli", "-f", "NAME,UUID,TYPE,DEVICE,AUTOCONNECT",
        "connection", "show", NULL};

    show_factory_baseline_status();
    if (!baseline_valid() || !baseline_archive(archive, sizeof(archive)))
    {
        return;
    }
    printf("\n--- 当前 NetworkManager 配置 ---\n");
    (void)run_cmd(list_current);
    printf("\n--- 初始基准中的配置文件 ---\n");
    list_archive[0] = "tar";
    list_archive[1] = "-tf";
    list_archive[2] = archive;
    list_archive[3] = NULL;
    (void)run_cmd(list_archive);
    printf("\n恢复会用基准目录替换当前持久化连接文件；当前运行中的连接在重启前不主动切断。\n");
}

static bool unlink_connection_files(const char *directory)
{
    DIR *handle = opendir(directory);
    struct dirent *entry;
    int directory_fd;

    if (!handle)
    {
        return false;
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
                    AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(info.st_mode) ||
            unlinkat(directory_fd, entry->d_name, 0) != 0)
        {
            closedir(handle);
            return false;
        }
    }
    return closedir(handle) == 0;
}

void restore_factory_network(void)
{
    char archive[PATH_MAX];
    char nm_connections[PATH_MAX];
    char backups[PATH_MAX];
    char emergency[PATH_MAX];
    char stamp[32];
    char camera_current[PATH_MAX];
    char camera_baseline[PATH_MAX];
    char camera_absent[PATH_MAX];
    char camera_transaction[PATH_MAX];
    char emergency_camera[PATH_MAX];
    bool had_camera;
    time_t now = time(NULL);
    struct tm local;
    const char *backup_args[9];
    const char *restore_args[8];
    const char *emergency_restore_args[9];

    preview_factory_restore();
    if (!baseline_valid() || !baseline_archive(archive, sizeof(archive)) ||
        !join_path(nm_connections, sizeof(nm_connections),
                   nm_parent(), NM_CONNECTION_DIR) ||
        !current_camera_file(camera_current, sizeof(camera_current)) ||
        !baseline_camera_file(camera_baseline, sizeof(camera_baseline)) ||
        !baseline_camera_absent(camera_absent, sizeof(camera_absent)) ||
        !camera_transaction_file(camera_transaction,
                                 sizeof(camera_transaction)))
    {
        printf("[拒绝] 没有可信初始基准，不能恢复。\n");
        return;
    }
    printf("\n[高风险永久操作] 重启后当前网络地址、Wi-Fi 和 SSH 路径可能全部改变。\n");
    if (!read_exact_word("确认永久恢复请输入 FACTORY RESET: ",
                         "FACTORY RESET"))
    {
        printf("已取消。\n");
        return;
    }
    if (!join_path(backups, sizeof(backups), state_dir(), "backups") ||
        !ensure_directory(backups, 0700))
    {
        printf("[失败] 无法创建紧急备份目录。\n");
        return;
    }
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    if (snprintf(emergency, sizeof(emergency),
                 "%s/before-factory-reset-%s.tar", backups, stamp) < 0)
    {
        return;
    }
    if (snprintf(emergency_camera, sizeof(emergency_camera),
                 "%s/before-factory-reset-%s.camera.json", backups, stamp) < 0)
    {
        return;
    }
    had_camera = file_exists_regular(camera_current);
    backup_args[0] = "tar";
    backup_args[1] = "--create";
    backup_args[2] = "--file";
    backup_args[3] = emergency;
    backup_args[4] = "--directory";
    backup_args[5] = nm_parent();
    backup_args[6] = NM_CONNECTION_DIR;
    backup_args[7] = NULL;
    if (run_cmd(backup_args) != 0 || chmod(emergency, 0600) != 0)
    {
        printf("[失败] 紧急备份失败，未修改当前配置。\n");
        (void)unlink(emergency);
        return;
    }
    if (had_camera && !copy_text_file(camera_current, emergency_camera))
    {
        printf("[失败] 摄像头应用配置紧急备份失败，未修改当前配置。\n");
        return;
    }
    emergency_restore_args[0] = "tar";
    emergency_restore_args[1] = "--extract";
    emergency_restore_args[2] = "--overwrite";
    emergency_restore_args[3] = "--file";
    emergency_restore_args[4] = emergency;
    emergency_restore_args[5] = "--directory";
    emergency_restore_args[6] = nm_parent();
    emergency_restore_args[7] = NULL;
    if (!unlink_connection_files(nm_connections))
    {
        printf("[失败] 无法安全清理当前连接文件，正在立即恢复紧急备份。\n");
        (void)run_cmd(emergency_restore_args);
        printf("紧急备份位于 %s\n", emergency);
        return;
    }
    restore_args[0] = "tar";
    restore_args[1] = "--extract";
    restore_args[2] = "--file";
    restore_args[3] = archive;
    restore_args[4] = "--directory";
    restore_args[5] = nm_parent();
    restore_args[6] = NULL;
    if (run_cmd(restore_args) != 0)
    {
        printf("[严重] 初始配置解包失败，正在立即恢复操作前配置。\n");
        (void)unlink_connection_files(nm_connections);
        if (run_cmd(emergency_restore_args) != 0)
        {
            printf("[严重] 自动恢复也失败。不要重启；紧急备份位于 %s\n", emergency);
        }
        return;
    }
    if (file_exists_regular(camera_baseline))
    {
        (void)unlink(camera_current);
        if (!copy_text_file(camera_baseline, camera_current))
        {
            printf("[严重] 摄像头初始配置恢复失败，正在恢复操作前配置。\n");
            (void)unlink(camera_current);
            if (had_camera)
            {
                (void)copy_text_file(emergency_camera, camera_current);
            }
            (void)unlink_connection_files(nm_connections);
            (void)run_cmd(emergency_restore_args);
            return;
        }
    }
    else if (file_exists_regular(camera_absent))
    {
        if (unlink(camera_current) != 0 && errno != ENOENT)
        {
            printf("[严重] 无法清除摄像头应用配置，正在恢复操作前配置。\n");
            (void)unlink_connection_files(nm_connections);
            (void)run_cmd(emergency_restore_args);
            return;
        }
    }
    (void)unlink(camera_transaction);
    printf("\n[完成] NetworkManager 与摄像头应用的初始持久化网络配置已经写回。\n");
    printf("当前活动连接未被主动中断；请在有本地串口/显示器保障时自行重启。\n");
    printf("重启后初始配置生效。恢复前紧急备份：%s\n", emergency);
}
