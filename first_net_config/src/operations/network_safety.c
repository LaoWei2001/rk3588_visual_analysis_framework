#define _POSIX_C_SOURCE 200809L

#include "operations/network_safety.h"
#include "common/cli_io.h"
#include "common/command_runner.h"
#include "common/ipv4_utils.h"
#include "inspection/network_state.h"
#include "inspection/interface_inspector.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FIRST_NET_CONFIG_RUNTIME_DIR
#define FIRST_NET_CONFIG_RUNTIME_DIR "/run/first_net_config"
#endif

#define PENDING_STATE_MAGIC "FNC-PENDING-v2"
#define PENDING_STATE_VERSION 2U

typedef enum
{
    PENDING_STATUS_PREPARING = 1,
    PENDING_STATUS_AWAITING_CONFIRMATION,
    PENDING_STATUS_ROLLING_BACK,
    PENDING_STATUS_ROLLBACK_FAILED,
    PENDING_STATUS_CONFIRMING,
    PENDING_STATUS_SAVE_FAILED,
    PENDING_STATUS_CONFIRMED
} PendingStatus;

typedef struct
{
    char magic[16];
    uint32_t version;
    uint32_t struct_size;
    int32_t status;
    uint32_t profile_mode;
    int64_t created_at;
    int64_t deadline;
    char transaction_id[UUID_SIZE];
    char iface[IF_NAMESIZE];
    ConnectionProfile new_profile;
    ConnectionProfile old_profile;
    IPv4Config cfg;
    char final_profile_name[PROFILE_SIZE];
} PendingNetworkChange;

typedef enum
{
    PENDING_READ_ERROR = -1,
    PENDING_READ_NONE = 0,
    PENDING_READ_OK = 1
} PendingReadResult;

static bool rollback_pending_connection(const PendingNetworkChange *state,
                                        bool verbose);

static bool make_runtime_path(char *out,
                              size_t out_size,
                              const char *name)
{
    int written = snprintf(out, out_size, "%s/%s",
                           FIRST_NET_CONFIG_RUNTIME_DIR, name);

    return written >= 0 && (size_t)written < out_size;
}

static bool ensure_runtime_directory(void)
{
    struct stat info;

    if (mkdir(FIRST_NET_CONFIG_RUNTIME_DIR, 0700) != 0 && errno != EEXIST)
    {
        return false;
    }

    if (lstat(FIRST_NET_CONFIG_RUNTIME_DIR, &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != geteuid())
    {
        errno = EPERM;
        return false;
    }

    if ((info.st_mode & 0777) != 0700 &&
        chmod(FIRST_NET_CONFIG_RUNTIME_DIR, 0700) != 0)
    {
        return false;
    }

    return true;
}

static int lock_pending_state(void)
{
    char path[PATH_MAX];
    int fd;

    if (!ensure_runtime_directory() ||
        !make_runtime_path(path, sizeof(path), "pending.lock"))
    {
        return -1;
    }

    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
    {
        return -1;
    }

    if (flock(fd, LOCK_EX) != 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static void unlock_pending_state(int fd)
{
    if (fd >= 0)
    {
        (void)flock(fd, LOCK_UN);
        close(fd);
    }
}

static bool valid_uuid_text_local(const char *uuid)
{
    if (!uuid || strnlen(uuid, UUID_SIZE) != UUID_SIZE - 1)
    {
        return false;
    }

    for (size_t i = 0; i < UUID_SIZE - 1; ++i)
    {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        bool hex = (uuid[i] >= '0' && uuid[i] <= '9') ||
                   (uuid[i] >= 'a' && uuid[i] <= 'f') ||
                   (uuid[i] >= 'A' && uuid[i] <= 'F');

        if ((dash && uuid[i] != '-') || (!dash && !hex))
        {
            return false;
        }
    }

    return true;
}

static bool pending_state_is_valid(const PendingNetworkChange *state)
{
    if (!state ||
        memcmp(state->magic, PENDING_STATE_MAGIC,
               sizeof(PENDING_STATE_MAGIC)) != 0 ||
        state->version != PENDING_STATE_VERSION ||
        state->struct_size != sizeof(*state) ||
        state->status < PENDING_STATUS_PREPARING ||
        state->status > PENDING_STATUS_CONFIRMED ||
        state->profile_mode > NETWORK_PROFILE_EDIT ||
        !memchr(state->transaction_id, '\0', sizeof(state->transaction_id)) ||
        !memchr(state->iface, '\0', sizeof(state->iface)) ||
        !memchr(state->new_profile.name, '\0',
                sizeof(state->new_profile.name)) ||
        !memchr(state->new_profile.uuid, '\0',
                sizeof(state->new_profile.uuid)) ||
        !memchr(state->old_profile.name, '\0',
                sizeof(state->old_profile.name)) ||
        !memchr(state->old_profile.uuid, '\0',
                sizeof(state->old_profile.uuid)) ||
        !memchr(state->final_profile_name, '\0',
                sizeof(state->final_profile_name)) ||
        !valid_uuid_text_local(state->transaction_id) ||
        !valid_uuid_text_local(state->new_profile.uuid) ||
        state->iface[0] == '\0' || state->new_profile.name[0] == '\0')
    {
        return false;
    }

    if (state->old_profile.uuid[0] != '\0' &&
        !valid_uuid_text_local(state->old_profile.uuid))
    {
        return false;
    }

    if ((state->profile_mode == NETWORK_PROFILE_PERMANENT ||
         state->profile_mode == NETWORK_PROFILE_EDIT) &&
        state->final_profile_name[0] == '\0')
    {
        return false;
    }

    if (state->cfg.is_static && !valid_ipv4(state->cfg.ip))
    {
        return false;
    }

    return true;
}

static PendingReadResult read_pending_state_locked(PendingNetworkChange *state)
{
    char path[PATH_MAX];
    struct stat info;
    int fd;
    size_t used = 0;

    if (!state || !make_runtime_path(path, sizeof(path), "pending.state"))
    {
        return PENDING_READ_ERROR;
    }

    memset(state, 0, sizeof(*state));
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
    {
        return errno == ENOENT ? PENDING_READ_NONE : PENDING_READ_ERROR;
    }

    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_size != (off_t)sizeof(*state))
    {
        close(fd);
        return PENDING_READ_ERROR;
    }

    while (used < sizeof(*state))
    {
        ssize_t count = read(fd, (char *)state + used,
                             sizeof(*state) - used);

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            close(fd);
            return PENDING_READ_ERROR;
        }
        used += (size_t)count;
    }

    close(fd);
    return pending_state_is_valid(state) ? PENDING_READ_OK
                                         : PENDING_READ_ERROR;
}

static bool write_all(int fd, const void *data, size_t size)
{
    const char *cursor = data;

    while (size > 0)
    {
        ssize_t count = write(fd, cursor, size);

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }

        cursor += count;
        size -= (size_t)count;
    }

    return true;
}

static bool write_pending_state_locked(const PendingNetworkChange *state)
{
    char path[PATH_MAX];
    char temporary[PATH_MAX];
    int fd;
    int written;
    bool ok;

    if (!pending_state_is_valid(state) ||
        !make_runtime_path(path, sizeof(path), "pending.state"))
    {
        return false;
    }

    written = snprintf(temporary, sizeof(temporary),
                       "%s/pending.tmp.%ld",
                       FIRST_NET_CONFIG_RUNTIME_DIR, (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temporary))
    {
        return false;
    }

    fd = open(temporary,
              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0)
    {
        return false;
    }

    ok = write_all(fd, state, sizeof(*state)) && fsync(fd) == 0;
    if (close(fd) != 0)
    {
        ok = false;
    }

    if (ok && rename(temporary, path) == 0)
    {
        return true;
    }

    (void)unlink(temporary);
    return false;
}

static bool remove_pending_state_locked(void)
{
    char path[PATH_MAX];

    if (!make_runtime_path(path, sizeof(path), "pending.state"))
    {
        return false;
    }

    return unlink(path) == 0 || errno == ENOENT;
}

static bool create_pending_state(const char *iface,
                                 const ConnectionProfile *new_profile,
                                 const ConnectionProfile *old_profile,
                                 const IPv4Config *cfg,
                                 const char *final_profile_name,
                                 NetworkProfileMode profile_mode,
                                 PendingNetworkChange *out)
{
    PendingNetworkChange existing;
    PendingReadResult read_result;
    int lock_fd;
    time_t now = time(NULL);

    if (!iface || !new_profile || !cfg || !out)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->magic, sizeof(out->magic), "%s", PENDING_STATE_MAGIC);
    out->version = PENDING_STATE_VERSION;
    out->struct_size = sizeof(*out);
    out->status = PENDING_STATUS_PREPARING;
    out->profile_mode = (uint32_t)profile_mode;
    out->created_at = (int64_t)now;
    out->deadline = (int64_t)now + ACTIVATION_ROLLBACK_TIMEOUT_SEC;
    snprintf(out->iface, sizeof(out->iface), "%s", iface);
    out->new_profile = *new_profile;
    if (old_profile)
    {
        out->old_profile = *old_profile;
    }
    out->cfg = *cfg;
    snprintf(out->final_profile_name, sizeof(out->final_profile_name), "%s",
             final_profile_name ? final_profile_name : "");

    if (!generate_connection_uuid(out->transaction_id,
                                  sizeof(out->transaction_id)) ||
        !pending_state_is_valid(out))
    {
        return false;
    }

    lock_fd = lock_pending_state();
    if (lock_fd < 0)
    {
        return false;
    }

    read_result = read_pending_state_locked(&existing);
    if (read_result == PENDING_READ_OK &&
        existing.status == PENDING_STATUS_CONFIRMED)
    {
        (void)remove_pending_state_locked();
        read_result = PENDING_READ_NONE;
    }

    if (read_result != PENDING_READ_NONE)
    {
        unlock_pending_state(lock_fd);
        printf("[失败] 已存在尚未处理完的网络切换事务。请重新运行工具处理它。\n");
        return false;
    }

    if (!write_pending_state_locked(out))
    {
        unlock_pending_state(lock_fd);
        return false;
    }

    unlock_pending_state(lock_fd);
    return true;
}

static int ask_ipv4_mode(void)
{
    printf("\nIP 地址设置方式：\n");
    printf("  1. 自动获取 IP 地址\n");
    printf("  2. 手动填写固定 IP 地址\n");
    printf("  0. 取消本次配置\n");

    {
        int choice = read_int("请选择 [0-2]: ", 0, 2);
        return choice == 0 ? TERMINAL_UI_INPUT_CANCELLED : choice;
    }
}

int read_ipv4_route_metric(void)
{
    int choice;

route_choice:
    terminal_ui_set_step("设置联网优先级",
                         "数值越小，系统越优先使用这项连接");
    printf("\n联网优先级（数值越小越优先）：\n");
    printf("  1. 自动：由系统按网卡类型决定\n");
    printf("  2. 主用：%d\n", IPV4_ROUTE_METRIC_PRIMARY);
    printf("  3. 备用：%d\n", IPV4_ROUTE_METRIC_BACKUP);
    printf("  4. 自定义：适合已有明确网络规划的现场\n");
    printf("  0. 取消本次配置\n");
    printf("同一内网同时使用有线和 Wi-Fi 时，请给两者设置不同优先级。\n");

    choice = read_int("请选择 [0-4]: ", 0, 4);
    if (choice == 0 || choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        return TERMINAL_UI_INPUT_CANCELLED;
    }
    if (choice == 1)
    {
        return IPV4_ROUTE_METRIC_AUTO;
    }
    if (choice == 2)
    {
        return IPV4_ROUTE_METRIC_PRIMARY;
    }
    if (choice == 3)
    {
        return IPV4_ROUTE_METRIC_BACKUP;
    }
    terminal_ui_set_step("填写自定义优先级",
                         "请输入 1 到 9999 之间的数值");
    terminal_ui_prepare_step(true);
    choice = read_int("请输入自定义优先级 [1-9999]: ", 1, 9999);
    if (choice == TERMINAL_UI_INPUT_CANCELLED &&
        terminal_ui_back_requested())
    {
        goto route_choice;
    }
    return choice;
}

bool change_route_metric_interactively(const char *profile_uuid,
                                       const char *profile_name,
                                       const char *iface,
                                       const char *current_metric)
{
    char metric_text[16];
    int metric;
    const char *display_name = profile_name && profile_name[0]
                                   ? profile_name
                                   : profile_uuid;

    if (!profile_uuid || profile_uuid[0] == '\0' || !display_name)
    {
        printf("[失败] 无法确定要修改的 NetworkManager 连接。\n");
        return false;
    }

    terminal_ui_set_step("填写路由优先级",
                         "只修改刚才明确选择的连接，数值越小越优先");
    printf("\n准备修改：%s", display_name);
    if (iface && iface[0])
    {
        printf("（网卡 %s）", iface);
    }
    printf("\n当前优先级：%s\n",
           current_metric && current_metric[0] &&
                   strcmp(current_metric, "-1") != 0
               ? current_metric
               : "自动");
    printf("数值越小越优先；请输入 1 到 9999 之间的明确数值。\n");
    terminal_ui_prepare_step(true);
    metric = read_int("新的路由优先级 [1-9999]: ", 1, 9999);
    if (metric == TERMINAL_UI_INPUT_CANCELLED)
    {
        return false;
    }

    printf("\n即将只把“%s”的路由优先级改为 %d。\n",
           display_name, metric);
    if (iface && iface[0] && interface_is_ssh_path(iface))
    {
        printf("[当前 SSH] 此连接承载当前远程会话，修改后网络路径可能切换。\n");
    }
    if (iface && iface[0] && interface_has_default_route(iface))
    {
        printf("[默认联网] 此连接当前参与默认联网，修改可能改变出口选择。\n");
    }
    terminal_ui_set_step("确认修改路由优先级",
                         "不会自动修改其他连接或其他网络参数");
    terminal_ui_prepare_step(true);
    if (!read_exact_word("确认修改请输入 SET PRIORITY: ", "SET PRIORITY"))
    {
        printf("已取消，优先级没有变化。\n");
        return false;
    }

    snprintf(metric_text, sizeof(metric_text), "%d", metric);
    {
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", profile_uuid,
            "ipv4.route-metric", metric_text, NULL};

        if (run_cmd(modify) != 0)
        {
            printf("[失败] 无法保存新的路由优先级。\n");
            return false;
        }
    }
    if (iface && iface[0])
    {
        const char *reapply[] = {
            "nmcli", "device", "reapply", iface, NULL};

        if (run_cmd(reapply) != 0)
        {
            printf("[提醒] 优先级已经保存，将在下次连接时生效。\n");
        }
    }
    printf("[完成] “%s”的路由优先级已由用户设为 %d；"
           "其他连接未修改。\n",
           display_name, metric);
    return true;
}

int configure_ipv4_profile(const char *profile_uuid,
                           IPv4Config *cfg,
                           int *route_metric)
{
    int mode = 0;
    char metric_text[16];
    char mask[INET_ADDRSTRLEN] = {0};
    char entered[BUF_SIZE];
    char prompt[320];

    if (!profile_uuid || !cfg || !route_metric)
    {
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));

route_metric_step:
    terminal_ui_prepare_step(true);
    *route_metric = read_ipv4_route_metric();
    if (*route_metric == TERMINAL_UI_INPUT_CANCELLED)
    {
        return -1;
    }

address_mode_step:
    terminal_ui_set_step("选择 IPv4 方式",
                         "由路由器自动分配，或手工填写固定地址");
    terminal_ui_prepare_step(true);
    mode = ask_ipv4_mode();
    if (mode == TERMINAL_UI_INPUT_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto route_metric_step;
        }
        return -1;
    }

    if (mode == 1)
    {
        snprintf(metric_text, sizeof(metric_text), "%d", *route_metric);
        const char *argv[] = {
            "nmcli", "connection", "modify", "uuid", profile_uuid,
            "ipv4.method", "auto",
            "ipv4.addresses", "",
            "ipv4.gateway", "",
            "ipv4.dns", "",
            "ipv4.ignore-auto-dns", "no",
            "ipv4.route-metric", metric_text,
            "ipv4.dad-timeout", "-1",
            NULL};

        cfg->is_static = false;
        return run_cmd(argv);
    }

    cfg->is_static = true;

ipv4_address_step:
    terminal_ui_set_step("填写 IPv4 地址",
                         "填写设备在当前网络中使用的固定地址");
    printf("\n固定 IP 是盒子在当前网络中的固定地址；现场提供则原样填写。\n");

    for (;;)
    {
        snprintf(prompt, sizeof(prompt),
                 cfg->ip[0]
                     ? "IPv4 地址（当前 %s，回车保留）: "
                     : "IPv4 地址: ",
                 cfg->ip);
        terminal_ui_prepare_step(true);
        if (!read_line(prompt, entered, sizeof(entered)))
        {
            if (terminal_ui_back_requested())
            {
                goto address_mode_step;
            }
            return -1;
        }
        trim_space(entered);
        if (entered[0])
        {
            if (strlen(entered) >= sizeof(cfg->ip))
            {
                printf("IP 地址过长。\n");
                continue;
            }
            memcpy(cfg->ip, entered, strlen(entered) + 1);
        }
        if (valid_ipv4(cfg->ip))
            break;
        printf("IP 地址格式不正确，例如：10.202.30.29\n");
    }

subnet_mask_step:
    terminal_ui_set_step("填写子网掩码",
                         "用于确定当前局域网的地址范围");
    printf("\n子网掩码用于判断哪些设备与盒子同属一个局域网。\n");
    printf("现场提供则原样填写；未提供请向网络管理员确认，不建议猜测。\n");

    for (;;)
    {
        snprintf(prompt, sizeof(prompt),
                 mask[0]
                     ? "子网掩码（当前 %s，回车保留）: "
                     : "子网掩码: ",
                 mask);
        terminal_ui_prepare_step(true);
        if (!read_line(prompt, entered, sizeof(entered)))
        {
            if (terminal_ui_back_requested())
            {
                goto ipv4_address_step;
            }
            return -1;
        }
        trim_space(entered);
        if (entered[0])
        {
            if (strlen(entered) >= sizeof(mask))
            {
                printf("子网掩码过长。\n");
                continue;
            }
            memcpy(mask, entered, strlen(entered) + 1);
        }
        cfg->prefix = netmask_to_prefix(mask);
        if (cfg->prefix >= 0 &&
            !is_network_or_broadcast_address(cfg->ip, cfg->prefix))
        {
            break;
        }
        if (cfg->prefix < 0)
        {
            printf("子网掩码格式不正确，例如：255.255.255.0\n");
        }
        else
        {
            printf("这个 IP 不能作为普通设备地址，请检查 IP 和子网掩码。\n");
        }
    }

gateway_step:
    terminal_ui_set_step("填写默认网关",
                         "用于访问其他网段；仅局域网可留空");
    printf("\n网关是盒子访问局域网外设备时先把数据交给的地址。\n");
    printf("现场提供则原样填写；明确无网关可回车留空，不确定请先确认。\n");

    for (;;)
    {
        snprintf(prompt, sizeof(prompt),
                 cfg->gateway[0]
                     ? "默认网关（当前 %s；回车保留，输入 - 清空）: "
                     : "默认网关（可留空）: ",
                 cfg->gateway);
        terminal_ui_prepare_step(true);
        if (!read_line(prompt, entered, sizeof(entered)))
        {
            if (terminal_ui_back_requested())
            {
                goto subnet_mask_step;
            }
            return -1;
        }
        trim_space(entered);
        if (strcmp(entered, "-") == 0)
        {
            cfg->gateway[0] = '\0';
            break;
        }
        if (entered[0] || cfg->gateway[0] == '\0')
        {
            if (strlen(entered) >= sizeof(cfg->gateway))
            {
                printf("网关地址过长。\n");
                continue;
            }
            memcpy(cfg->gateway, entered, strlen(entered) + 1);
        }

        if (cfg->gateway[0] == '\0')
            break;
        if (!valid_ipv4(cfg->gateway))
        {
            printf("网关地址格式不正确，例如：10.202.30.254\n");
            continue;
        }
        if (!ipv4_same_subnet(cfg->ip, cfg->gateway, cfg->prefix))
        {
            printf("警告：网关 %s 与设备 IP %s/%d 不在同一局域网，通常为填写错误，请核对现场信息。\n",
                   cfg->gateway, cfg->ip, cfg->prefix);
            continue;
        }
        break;
    }

    terminal_ui_set_step("填写 DNS 服务器",
                         "用于解析域名；仅使用 IP 通信可留空");
    printf("\n域名服务器用于把网站名或服务器名转换成 IP 地址；仅用 IP 通信可回车留空。\n");
    for (;;)
    {
        snprintf(prompt, sizeof(prompt),
                 cfg->dns[0]
                     ? "DNS（当前 %s；回车保留，输入 - 清空）: "
                     : "DNS（可留空；多个地址用逗号分隔，最多 4 个）: ",
                 cfg->dns);
        terminal_ui_prepare_step(true);
        if (!read_line(prompt, entered, sizeof(entered)))
        {
            if (terminal_ui_back_requested())
            {
                goto gateway_step;
            }
            return -1;
        }
        trim_space(entered);
        if (strcmp(entered, "-") == 0)
        {
            cfg->dns[0] = '\0';
            break;
        }
        if (entered[0] == '\0' && cfg->dns[0])
        {
            break;
        }
        if (normalize_ipv4_list(entered, cfg->dns, sizeof(cfg->dns), 4))
        {
            break;
        }
        printf("DNS 地址格式不正确，请填写 IPv4 地址，多个地址用逗号分隔。\n");
    }

    {
        char cidr[32];
        snprintf(metric_text, sizeof(metric_text), "%d", *route_metric);
        snprintf(cidr, sizeof(cidr), "%s/%d", cfg->ip, cfg->prefix);
        const char *argv[] = {
            "nmcli", "connection", "modify", "uuid", profile_uuid,
            "ipv4.method", "manual",
            "ipv4.addresses", cidr,
            "ipv4.gateway", cfg->gateway,
            "ipv4.dns", cfg->dns,
            "ipv4.ignore-auto-dns", "yes",
            "ipv4.route-metric", metric_text,
            "ipv4.dad-timeout", IPV4_DAD_TIMEOUT_MS,
            NULL};
        if (run_cmd(argv) != 0)
            return -1;
    }

    printf("\n[安全检查] 已开启 IP 占用检查（最长 %s 毫秒）；若该 IP 已被占用，本次设置停止且不会保存。\n",
           IPV4_DAD_TIMEOUT_MS);
    return 0;
}

static bool validate_new_connection(const char *iface,
                                    const ConnectionProfile *profile,
                                    const IPv4Config *cfg)
{
    char active_uuid[UUID_SIZE] = {0};
    bool ok = true;

    printf("\n========== 正在检查新网络 ==========\n");

    if (get_active_connection_uuid(iface,
                                   active_uuid,
                                   sizeof(active_uuid)) &&
        strcmp(active_uuid, profile->uuid) == 0)
    {
        printf("[正常] 新网络配置已启用：%s\n", profile->name);
    }
    else
    {
        printf("[失败] 系统没有切换到刚刚设置的新网络。\n");
        if (active_uuid[0] != '\0')
        {
            char active_name[PROFILE_SIZE] = {0};
            if (get_connection_name(active_uuid,
                                    active_name,
                                    sizeof(active_name)))
            {
                printf("       当前仍启用：%s\n", active_name);
            }
        }
        ok = false;
    }

    if (cfg->is_static)
    {
        if (interface_has_ipv4(iface, cfg->ip))
        {
            printf("[正常] 固定 IP 已生效：%s/%d\n",
                   cfg->ip, cfg->prefix);
        }
        else
        {
            printf("[失败] 没有检测到你设置的 IP：%s\n",
                   cfg->ip);
            ok = false;
        }

        if (cfg->gateway[0] != '\0')
        {
            if (default_route_matches(iface, cfg->gateway))
            {
                printf("[正常] 网关已经生效：%s\n",
                       cfg->gateway);
            }
            else
            {
                printf("[失败] 没有检测到你设置的网关：%s\n",
                       cfg->gateway);
                ok = false;
            }
        }
        else
        {
            printf("[正常] 这次没有设置网关。\n");
        }
    }
    else
    {
        if (has_any_ipv4(iface))
        {
            printf("[正常] 已自动获得 IP 地址。\n");
        }
        else
        {
            printf("[失败] 还没有自动获得 IP 地址。\n");
            ok = false;
        }
    }

    return ok;
}

static void optional_gateway_ping(const IPv4Config *cfg)
{
    if (!cfg->is_static || cfg->gateway[0] == '\0')
    {
        return;
    }

    printf("\n正在测试能否连接网关 %s\n", cfg->gateway);
    printf("说明：有些网络不允许这种测试，因此测试不通不一定表示配置错误。\n");

    {
        const char *argv[] = {
            "ping", "-c", "3", "-W", "1",
            cfg->gateway, NULL};

        if (run_cmd_silent(argv) == 0)
        {
            printf("[正常] 可以连接到网关。\n");
        }
        else
        {
            printf("[提醒] 网关没有回应测试。本项只作参考，不会仅凭这一项判定失败。\n");
        }
    }
}

static void redirect_stdio_to_devnull(void)
{
    int fd = open("/dev/null", O_RDWR);

    if (fd < 0)
    {
        return;
    }

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    terminal_ui_detach();

    if (fd > STDERR_FILENO)
    {
        close(fd);
    }
}

static bool connection_state_matches(const char *iface,
                                     const ConnectionProfile *profile,
                                     const IPv4Config *cfg)
{
    char active_uuid[UUID_SIZE] = {0};

    if (!get_active_connection_uuid(iface,
                                    active_uuid,
                                    sizeof(active_uuid)) ||
        strcmp(active_uuid, profile->uuid) != 0)
    {
        return false;
    }

    if (!cfg->is_static)
    {
        return has_any_ipv4(iface);
    }

    if (!interface_has_ipv4(iface, cfg->ip))
    {
        return false;
    }

    return cfg->gateway[0] == '\0' ||
           default_route_matches(iface, cfg->gateway);
}

static int run_network_command(bool verbose, const char *const argv[])
{
    return verbose ? run_cmd(argv) : run_cmd_silent(argv);
}

static bool rollback_pending_connection(const PendingNetworkChange *state,
                                        bool verbose)
{
    bool restored = true;

    if (verbose)
    {
        printf("\n[正在恢复] 正在恢复网络切换前的状态...\n");
    }

    if (state->old_profile.uuid[0] != '\0' &&
        strcmp(state->old_profile.uuid, state->new_profile.uuid) != 0)
    {
        const char *up_old[] = {
            "nmcli", "--wait", "30",
            "connection", "up", "uuid", state->old_profile.uuid,
            "ifname", state->iface,
            NULL};

        restored = run_network_command(verbose, up_old) == 0;
        if (!restored)
        {
            const char *keep_new[] = {
                "nmcli", "--wait", NMCLI_UP_WAIT_SEC,
                "connection", "up", "uuid", state->new_profile.uuid,
                "ifname", state->iface,
                NULL};

            (void)run_network_command(verbose, keep_new);
            if (verbose)
            {
                printf("[失败] 无法重新启用修改前的网络：%s\n",
                       state->old_profile.name);
                printf("       为避免同时删除当前可用配置，新的测试连接暂时保留。\n");
            }
            return false;
        }
    }
    else if (state->old_profile.uuid[0] == '\0')
    {
        if (connection_is_active(state->new_profile.uuid))
        {
            const char *down_new[] = {
                "nmcli", "--wait", "15",
                "connection", "down", "uuid", state->new_profile.uuid,
                NULL};

            restored = run_network_command(verbose, down_new) == 0;
        }
    }

    if (state->profile_mode != NETWORK_PROFILE_EXISTING &&
        strcmp(state->old_profile.uuid, state->new_profile.uuid) != 0)
    {
        const char *remove_new[] = {
            "nmcli", "connection", "delete", "uuid",
            state->new_profile.uuid, NULL};

        if (run_network_command(verbose, remove_new) != 0)
        {
            char resolved[UUID_SIZE] = {0};

            if (resolve_connection_uuid(state->new_profile.uuid,
                                        resolved, sizeof(resolved)))
            {
                if (verbose)
                {
                    printf("[提醒] 原网络已经恢复，但测试连接未能自动删除：%s\n",
                           state->new_profile.name);
                }
                restored = false;
            }
        }
    }

    if (verbose && restored)
    {
        if (state->old_profile.uuid[0] != '\0')
        {
            printf("[恢复完成] 已重新启用修改前的网络：%s\n",
                   state->old_profile.name);
        }
        else
        {
            printf("[恢复完成] 已恢复为修改前未连接的状态。\n");
        }
    }

    return restored;
}

static bool rollback_pending_transaction(const char *transaction_id,
                                         bool force,
                                         bool verbose)
{
    PendingNetworkChange state;
    PendingReadResult read_result;
    int lock_fd = lock_pending_state();
    bool restored;

    if (lock_fd < 0)
    {
        return false;
    }

    read_result = read_pending_state_locked(&state);
    if (read_result == PENDING_READ_NONE)
    {
        unlock_pending_state(lock_fd);
        return true;
    }

    if (read_result != PENDING_READ_OK ||
        strcmp(state.transaction_id, transaction_id) != 0)
    {
        unlock_pending_state(lock_fd);
        return false;
    }

    if (state.status >= PENDING_STATUS_CONFIRMING)
    {
        unlock_pending_state(lock_fd);
        return false;
    }

    if (!force && state.deadline > (int64_t)time(NULL))
    {
        unlock_pending_state(lock_fd);
        return false;
    }

    state.status = PENDING_STATUS_ROLLING_BACK;
    (void)write_pending_state_locked(&state);
    restored = rollback_pending_connection(&state, verbose);

    if (restored)
    {
        (void)remove_pending_state_locked();
    }
    else
    {
        state.status = PENDING_STATUS_ROLLBACK_FAILED;
        (void)write_pending_state_locked(&state);
    }

    unlock_pending_state(lock_fd);
    return restored;
}

static void pending_watchdog_loop(const PendingNetworkChange *initial)
{
    for (;;)
    {
        PendingNetworkChange current;
        PendingReadResult read_result;
        struct timespec delay = {1, 0};
        int lock_fd;

        (void)nanosleep(&delay, NULL);
        lock_fd = lock_pending_state();
        if (lock_fd < 0)
        {
            if ((int64_t)time(NULL) >= initial->deadline)
            {
                (void)rollback_pending_connection(initial, false);
                return;
            }
            continue;
        }

        read_result = read_pending_state_locked(&current);
        unlock_pending_state(lock_fd);

        if (read_result == PENDING_READ_NONE ||
            (read_result == PENDING_READ_OK &&
             strcmp(current.transaction_id, initial->transaction_id) != 0))
        {
            return;
        }

        if (read_result == PENDING_READ_OK)
        {
            if (current.status >= PENDING_STATUS_CONFIRMING)
            {
                return;
            }

            if (current.status == PENDING_STATUS_ROLLBACK_FAILED)
            {
                return;
            }

            if (current.deadline > (int64_t)time(NULL))
            {
                continue;
            }

            (void)rollback_pending_transaction(current.transaction_id,
                                               false, false);
            return;
        }

        if ((int64_t)time(NULL) >= initial->deadline)
        {
            (void)rollback_pending_connection(initial, false);
            return;
        }
    }
}

static bool start_forked_pending_watchdog(const PendingNetworkChange *state)
{
    int ready_pipe[2];
    pid_t first_child;
    int status = 0;
    char ready = '0';

    if (pipe(ready_pipe) != 0)
    {
        return false;
    }

    first_child = fork();
    if (first_child < 0)
    {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return false;
    }

    if (first_child == 0)
    {
        pid_t daemon_child;

        close(ready_pipe[0]);
        if (setsid() < 0)
        {
            close(ready_pipe[1]);
            _exit(1);
        }

        daemon_child = fork();
        if (daemon_child < 0)
        {
            close(ready_pipe[1]);
            _exit(1);
        }

        if (daemon_child > 0)
        {
            close(ready_pipe[1]);
            _exit(0);
        }

        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        redirect_stdio_to_devnull();

        ready = '1';
        (void)write(ready_pipe[1], &ready, 1);
        close(ready_pipe[1]);
        pending_watchdog_loop(state);
        _exit(0);
    }

    close(ready_pipe[1]);
    (void)waitpid(first_child, &status, 0);

    {
        ssize_t count;
        do
        {
            count = read(ready_pipe[0], &ready, 1);
        } while (count < 0 && errno == EINTR);

        close(ready_pipe[0]);
        return count == 1 && ready == '1' &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
}

int run_pending_network_watchdog(const char *transaction_id)
{
    PendingNetworkChange state;
    int lock_fd;

    if (!valid_uuid_text_local(transaction_id))
    {
        return 2;
    }

    lock_fd = lock_pending_state();
    if (lock_fd < 0)
    {
        return 1;
    }

    if (read_pending_state_locked(&state) != PENDING_READ_OK ||
        strcmp(state.transaction_id, transaction_id) != 0)
    {
        unlock_pending_state(lock_fd);
        return 1;
    }

    unlock_pending_state(lock_fd);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    pending_watchdog_loop(&state);
    return 0;
}

static bool start_systemd_pending_watchdog(
    const PendingNetworkChange *state)
{
    char executable[PATH_MAX];
    char unit[128];
    ssize_t executable_size;
    int unit_size;

    if (access("/run/systemd/system", F_OK) != 0)
    {
        return false;
    }

    executable_size = readlink("/proc/self/exe", executable,
                               sizeof(executable) - 1);
    if (executable_size <= 0)
    {
        return false;
    }
    executable[executable_size] = '\0';

    unit_size = snprintf(unit, sizeof(unit),
                         "first-net-config-watchdog-%s",
                         state->transaction_id);
    if (unit_size < 0 || (size_t)unit_size >= sizeof(unit))
    {
        return false;
    }

    {
        const char *start[] = {
            "systemd-run", "--quiet", "--collect",
            "--unit", unit,
            executable, "--watch-pending", state->transaction_id,
            NULL};

        if (run_cmd_silent(start) != 0)
        {
            return false;
        }
    }

    for (int attempt = 0; attempt < 10; ++attempt)
    {
        const char *active[] = {
            "systemctl", "is-active", "--quiet", unit, NULL};
        struct timespec delay = {0, 100L * 1000L * 1000L};

        if (run_cmd_silent(active) == 0)
        {
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }

    return false;
}

static bool start_pending_watchdog(const PendingNetworkChange *state)
{
    if (start_systemd_pending_watchdog(state))
    {
        return true;
    }

    return start_forked_pending_watchdog(state);
}

static bool mark_pending_awaiting(const char *transaction_id)
{
    PendingNetworkChange state;
    int lock_fd = lock_pending_state();
    bool ok = false;

    if (lock_fd < 0)
    {
        return false;
    }

    if (read_pending_state_locked(&state) == PENDING_READ_OK &&
        strcmp(state.transaction_id, transaction_id) == 0 &&
        (state.status == PENDING_STATUS_PREPARING ||
         state.status == PENDING_STATUS_AWAITING_CONFIRMATION))
    {
        state.status = PENDING_STATUS_AWAITING_CONFIRMATION;
        state.deadline = (int64_t)time(NULL) + PENDING_CONFIRM_TIMEOUT_SEC;
        ok = write_pending_state_locked(&state);
    }

    unlock_pending_state(lock_fd);
    return ok;
}

static void print_reconnect_command(void)
{
    char executable[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", executable,
                            sizeof(executable) - 1);

    if (size > 0)
    {
        executable[size] = '\0';
        printf("  sudo %s\n", executable);
    }
    else
    {
        printf("  sudo ./first_net_config\n");
    }
}

static void print_pending_summary(const PendingNetworkChange *state)
{
    int64_t remaining = state->deadline - (int64_t)time(NULL);

    if (remaining < 0)
    {
        remaining = 0;
    }

    printf("\n============================================================\n");
    printf("                 检测到待确认网络配置\n");
    printf("============================================================\n");
    printf("网卡       : %s\n", state->iface);
    printf("测试连接   : %s\n", state->new_profile.name);
    if (state->cfg.is_static)
    {
        printf("新 IP      : %s/%d\n", state->cfg.ip, state->cfg.prefix);
    }
    else
    {
        printf("新 IP      : DHCP 自动获取\n");
    }
    if (state->profile_mode == NETWORK_PROFILE_PERMANENT ||
        state->profile_mode == NETWORK_PROFILE_EDIT)
    {
        printf("正式名称   : %s\n", state->final_profile_name);
    }
    printf("保存方式   : %s\n",
           state->profile_mode == NETWORK_PROFILE_PERMANENT
               ? "永久（重启后保留）"
               : state->profile_mode == NETWORK_PROFILE_TEMPORARY
                     ? "临时（重启后消失）"
                     : state->profile_mode == NETWORK_PROFILE_EDIT
                           ? "修改已有连接（保留原自动连接策略）"
                           : "已有连接测试");
    printf("剩余时间   : %lld 秒\n", (long long)remaining);
    printf("事务编号   : %s\n", state->transaction_id);
    printf("============================================================\n");
}

static bool complete_pending_save(PendingNetworkChange *state)
{
    bool saved;
    int lock_fd = lock_pending_state();

    if (lock_fd < 0)
    {
        printf("[严重提醒] 无法更新待确认事务状态，请不要立即重启设备。\n");
        return false;
    }

    {
        PendingNetworkChange current;

        if (read_pending_state_locked(&current) != PENDING_READ_OK ||
            strcmp(current.transaction_id, state->transaction_id) != 0 ||
            (current.status != PENDING_STATUS_CONFIRMING &&
             current.status != PENDING_STATUS_SAVE_FAILED))
        {
            unlock_pending_state(lock_fd);
            return false;
        }

        current.status = PENDING_STATUS_CONFIRMING;
        current.deadline = 0;
        if (!write_pending_state_locked(&current))
        {
            unlock_pending_state(lock_fd);
            printf("[严重提醒] 无法锁定正式保存状态，请不要立即重启设备。\n");
            return false;
        }

        *state = current;
        if (current.profile_mode == NETWORK_PROFILE_PERMANENT)
        {
            saved = finalize_profile(&current.new_profile,
                                     current.final_profile_name,
                                     &current.old_profile);
        }
        else if (current.profile_mode == NETWORK_PROFILE_EDIT)
        {
            saved = finalize_edited_profile(&current.new_profile,
                                            current.final_profile_name,
                                            &current.old_profile);
        }
        else
        {
            saved = true;
        }

        current.status = saved ? PENDING_STATUS_CONFIRMED
                               : PENDING_STATUS_SAVE_FAILED;
        current.deadline = 0;
        if (!write_pending_state_locked(&current))
        {
            unlock_pending_state(lock_fd);
            printf("[严重提醒] 无法持久记录确认结果，请不要立即重启设备。\n");
            return false;
        }

        *state = current;
        if (saved)
        {
            (void)remove_pending_state_locked();
        }
    }

    unlock_pending_state(lock_fd);

    if (!saved)
    {
        printf("\n[提醒] 已停止自动回退，但正式保存尚未完成。\n");
        printf("请保持设备运行并再次启动本工具，程序会继续完成保存。\n");
        return false;
    }

    printf("\n[完成] 新网络已经确认，自动回退保护已取消。\n");
    return true;
}

static bool confirm_pending_transaction(const char *transaction_id)
{
    PendingNetworkChange state;
    int lock_fd;

    lock_fd = lock_pending_state();
    if (lock_fd < 0)
    {
        return false;
    }

    if (read_pending_state_locked(&state) != PENDING_READ_OK ||
        strcmp(state.transaction_id, transaction_id) != 0 ||
        state.status != PENDING_STATUS_AWAITING_CONFIRMATION)
    {
        unlock_pending_state(lock_fd);
        return false;
    }

    if (state.deadline <= (int64_t)time(NULL))
    {
        unlock_pending_state(lock_fd);
        printf("[失败] 确认期限已经结束，不能再保留新配置。\n");
        (void)rollback_pending_transaction(transaction_id, true, true);
        return false;
    }

    if (!connection_state_matches(state.iface,
                                  &state.new_profile,
                                  &state.cfg))
    {
        unlock_pending_state(lock_fd);
        printf("[失败] 当前活动网络与待确认配置不一致，拒绝提交。\n");
        return false;
    }

    state.status = PENDING_STATUS_CONFIRMING;
    state.deadline = 0;
    if (!write_pending_state_locked(&state))
    {
        unlock_pending_state(lock_fd);
        printf("[失败] 无法安全停止自动回退，本次确认未提交。\n");
        return false;
    }

    unlock_pending_state(lock_fd);
    return complete_pending_save(&state);
}

typedef enum
{
    PENDING_PROMPT_LEFT_PENDING = 0,
    PENDING_PROMPT_CONFIRMED,
    PENDING_PROMPT_ROLLED_BACK
} PendingPromptResult;

static PendingPromptResult prompt_pending_decision(
    const PendingNetworkChange *state)
{
    char decision[32];

    print_pending_summary(state);
    printf("如果这是切换后的新 SSH 会话，说明新 IP 已经能够管理设备。\n");
    printf("输入 YES：正式保留并保存新配置。\n");
    printf("输入 ROLLBACK：立即恢复修改前的网络。\n");
    printf("直接退出或输入其他内容：保持待确认，超时后自动恢复。\n\n");

    terminal_ui_set_step("处理待确认网络",
                         "保留新网络，或立即恢复修改前的连接");
    terminal_ui_prepare_step(false);
    if (!read_line("请选择（YES/ROLLBACK）: ", decision, sizeof(decision)))
    {
        return PENDING_PROMPT_LEFT_PENDING;
    }
    trim_space(decision);

    if (strcmp(decision, "YES") == 0)
    {
        return confirm_pending_transaction(state->transaction_id)
                   ? PENDING_PROMPT_CONFIRMED
                   : PENDING_PROMPT_LEFT_PENDING;
    }

    if (strcmp(decision, "ROLLBACK") == 0)
    {
        return rollback_pending_transaction(state->transaction_id,
                                            true, true)
                   ? PENDING_PROMPT_ROLLED_BACK
                   : PENDING_PROMPT_LEFT_PENDING;
    }

    printf("\n[待确认] 没有提交新配置，自动回退倒计时仍在运行。\n");
    return PENDING_PROMPT_LEFT_PENDING;
}

static bool resume_confirmed_save(PendingNetworkChange *state)
{
    printf("[继续保存] 这项网络已经由你确认，正在继续完成正式保存。\n");
    return complete_pending_save(state);
}

bool handle_pending_network_change(void)
{
    PendingNetworkChange state;
    PendingReadResult read_result;
    int lock_fd = lock_pending_state();

    if (lock_fd < 0)
    {
        printf("[提醒] 无法检查跨会话网络事务；为了安全，暂不允许修改网络。\n");
        return true;
    }

    read_result = read_pending_state_locked(&state);
    unlock_pending_state(lock_fd);

    if (read_result == PENDING_READ_NONE)
    {
        return false;
    }

    if (read_result != PENDING_READ_OK)
    {
        printf("[失败] 待确认网络事务文件损坏，不能安全开始新的网络修改。\n");
        printf("请通过本地终端检查目录：%s\n",
               FIRST_NET_CONFIG_RUNTIME_DIR);
        return true;
    }

    if (state.status == PENDING_STATUS_CONFIRMED)
    {
        lock_fd = lock_pending_state();
        if (lock_fd >= 0)
        {
            (void)remove_pending_state_locked();
            unlock_pending_state(lock_fd);
        }
        printf("[完成] 上一次网络切换已经确认并保存。\n");
        return true;
    }

    if (state.status == PENDING_STATUS_CONFIRMING ||
        state.status == PENDING_STATUS_SAVE_FAILED)
    {
        (void)resume_confirmed_save(&state);
        return true;
    }

    if (state.status == PENDING_STATUS_ROLLING_BACK ||
        state.status == PENDING_STATUS_ROLLBACK_FAILED ||
        state.deadline <= (int64_t)time(NULL))
    {
        printf("[安全保护] 待确认配置已经超时或上次恢复未完成，正在恢复旧网络。\n");
        (void)rollback_pending_transaction(state.transaction_id,
                                           true, true);
        return true;
    }

    if (state.status == PENDING_STATUS_PREPARING)
    {
        if (connection_state_matches(state.iface,
                                     &state.new_profile,
                                     &state.cfg) &&
            validate_new_connection(state.iface,
                                    &state.new_profile,
                                    &state.cfg) &&
            mark_pending_awaiting(state.transaction_id))
        {
            lock_fd = lock_pending_state();
            if (lock_fd >= 0 &&
                read_pending_state_locked(&state) == PENDING_READ_OK)
            {
                unlock_pending_state(lock_fd);
                (void)prompt_pending_decision(&state);
                return true;
            }
            unlock_pending_state(lock_fd);
        }

        printf("[安全保护] 网络切换仍在进行或尚未通过本机检查。\n");
        printf("请稍后再次运行本工具；超时后程序会自动恢复旧网络。\n");
        return true;
    }

    (void)prompt_pending_decision(&state);
    return true;
}

NetworkActivationResult safe_activate_with_reconnect(
    const char *iface,
    const ConnectionProfile *profile,
    const IPv4Config *cfg,
    const char *final_profile_name,
    NetworkProfileMode profile_mode)
{
    ConnectionProfile old_profile = {0};
    PendingNetworkChange state;
    PendingPromptResult decision;

    terminal_ui_set_step("确认并应用网络",
                         "核对保存方式和断线自动回退提醒");

    if (get_active_connection_uuid(iface,
                                   old_profile.uuid,
                                   sizeof(old_profile.uuid)))
    {
        (void)get_connection_name(old_profile.uuid,
                                  old_profile.name,
                                  sizeof(old_profile.name));
    }

    printf("\n============================================================\n");
    printf("                   应用网络前的提醒\n");
    printf("============================================================\n");
    if (old_profile.uuid[0] != '\0')
    {
        printf("修改前，%s 已启用：%s\n", iface, old_profile.name);
    }
    else
    {
        printf("修改前，%s 没有已启用的保存连接。\n", iface);
    }
    printf("程序会先启用新设置，并启动自动恢复保护。\n");
    printf("若 SSH 因 IP 切换断开，请用新 IP 登录，并在 %d 秒内重跑本工具，将优先显示待确认配置。\n",
           PENDING_CONFIRM_TIMEOUT_SEC);
    if (cfg->is_static)
    {
        printf("重新连接地址：%s\n", cfg->ip);
    }
    printf("重新登录后运行：\n");
    print_reconnect_command();
    printf("没有输入 YES 确认时，程序仍会自动恢复修改前的网络。\n");
    printf("============================================================\n");

    if (profile_mode == NETWORK_PROFILE_PERMANENT)
    {
        printf("保存方式：永久配置，确认后重启仍会保留。\n");
        if (!read_exact_word("确认永久应用请输入 PERMANENT: ", "PERMANENT"))
        {
            return NETWORK_ACTIVATION_FAILED;
        }
    }
    else if (profile_mode == NETWORK_PROFILE_TEMPORARY)
    {
        printf("保存方式：临时配置，设备或网络服务重启后消失。\n");
        if (!read_exact_word("确认临时应用请输入 TEMPORARY: ", "TEMPORARY"))
        {
            return NETWORK_ACTIVATION_FAILED;
        }
    }
    else if (profile_mode == NETWORK_PROFILE_EDIT)
    {
        printf("保存方式：修改已有连接；确认后保留原来的自动连接策略。\n");
        if (!read_exact_word("确认应用修改请输入 MODIFY: ", "MODIFY"))
        {
            return NETWORK_ACTIVATION_FAILED;
        }
    }
    else if (!read_yes_no("确定使用这个已保存的连接吗？[y/N]: ", false))
    {
        printf("已取消。\n");
        return NETWORK_ACTIVATION_FAILED;
    }

    if (!create_pending_state(iface, profile, &old_profile, cfg,
                              final_profile_name,
                              profile_mode, &state))
    {
        printf("[失败] 无法建立持久化回退事务。为了避免设备失联，本次操作已取消。\n");
        return NETWORK_ACTIVATION_FAILED;
    }

    if (!start_pending_watchdog(&state))
    {
        int lock_fd = lock_pending_state();
        if (lock_fd >= 0)
        {
            (void)remove_pending_state_locked();
            unlock_pending_state(lock_fd);
        }
        printf("[失败] 无法启动独立的自动回退进程，本次操作已取消。\n");
        return NETWORK_ACTIVATION_FAILED;
    }

    printf("[安全保护] 自动回退进程已脱离当前 SSH 会话运行。\n");

    {
        const char *up_new[] = {
            "nmcli", "--wait", NMCLI_UP_WAIT_SEC,
            "connection", "up", "uuid", profile->uuid,
            "ifname", iface,
            NULL};

        if (run_cmd(up_new) != 0)
        {
            bool restored;

            printf("\n[失败] 新网络没有成功启用，正在恢复修改前的网络。\n");
            restored = rollback_pending_transaction(state.transaction_id,
                                                    true, true);
            return restored ? NETWORK_ACTIVATION_FAILED
                            : NETWORK_ACTIVATION_PENDING;
        }
    }

    if (!validate_new_connection(iface, profile, cfg))
    {
        bool restored;

        printf("\n[失败] 新网络检查没有通过，正在恢复修改前的网络。\n");
        restored = rollback_pending_transaction(state.transaction_id,
                                                true, true);
        return restored ? NETWORK_ACTIVATION_FAILED
                        : NETWORK_ACTIVATION_PENDING;
    }

    optional_gateway_ping(cfg);
    printf("\n========== 切换后的全接口网段检查 ==========\n");
    (void)show_current_overlap_warnings();

    if (!mark_pending_awaiting(state.transaction_id))
    {
        bool restored;

        printf("[提醒] 无法将事务标记为待确认，正在恢复修改前的网络。\n");
        restored = rollback_pending_transaction(state.transaction_id,
                                                true, true);
        return restored ? NETWORK_ACTIVATION_FAILED
                        : NETWORK_ACTIVATION_PENDING;
    }

    {
        int lock_fd = lock_pending_state();
        if (lock_fd < 0 ||
            read_pending_state_locked(&state) != PENDING_READ_OK)
        {
            unlock_pending_state(lock_fd);
            return NETWORK_ACTIVATION_PENDING;
        }
        unlock_pending_state(lock_fd);
    }

    decision = prompt_pending_decision(&state);
    if (decision == PENDING_PROMPT_CONFIRMED)
    {
        return NETWORK_ACTIVATION_CONFIRMED;
    }
    if (decision == PENDING_PROMPT_ROLLED_BACK)
    {
        return NETWORK_ACTIVATION_FAILED;
    }

    return NETWORK_ACTIVATION_PENDING;
}

static void make_backup_name(const char *final_name,
                             char *backup,
                             size_t backup_size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    snprintf(backup, backup_size,
             "%s.backup-%04d%02d%02d-%02d%02d%02d",
             final_name,
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec);
}

static bool read_connection_policy(const char *uuid,
                                   char *autoconnect,
                                   size_t autoconnect_size,
                                   char *priority,
                                   size_t priority_size)
{
    const char *read_autoconnect[] = {
        "nmcli", "-g", "connection.autoconnect",
        "connection", "show", "uuid", uuid, NULL};
    const char *read_priority[] = {
        "nmcli", "-g", "connection.autoconnect-priority",
        "connection", "show", "uuid", uuid, NULL};

    if (!uuid || !autoconnect || autoconnect_size == 0 ||
        !priority || priority_size == 0 ||
        capture_cmd(read_autoconnect, autoconnect, autoconnect_size) != 0 ||
        (strcmp(autoconnect, "yes") != 0 &&
         strcmp(autoconnect, "no") != 0) ||
        capture_cmd(read_priority, priority, priority_size) != 0)
    {
        return false;
    }
    if (priority[0] == '\0')
    {
        snprintf(priority, priority_size, "0");
    }
    return true;
}

static bool finalize_profile_internal(
    const ConnectionProfile *temp_profile,
    const char *final_profile_name,
    const ConnectionProfile *previous_profile,
    bool preserve_connection_policy)
{
    char backup[PROFILE_SIZE] = {0};
    char old_uuid[UUID_SIZE] = {0};
    char target_autoconnect[16] = "yes";
    char target_priority[32] = "100";
    int final_name_matches = find_connection_uuid_by_name(final_profile_name,
                                                           old_uuid,
                                                           sizeof(old_uuid));
    bool had_old_final;

    if (final_name_matches < 0)
    {
        printf("[失败] 无法读取现有网络配置，本次保存停止。\n");
        return false;
    }

    if (final_name_matches > 1)
    {
        printf("[失败] 已存在多个同名连接 \"%s\"。\n", final_profile_name);
        printf("为避免改错，请先清理重名连接。\n");
        return false;
    }

    had_old_final = final_name_matches == 1 &&
                    strcmp(old_uuid, temp_profile->uuid) != 0;

    if (preserve_connection_policy &&
        (!previous_profile || previous_profile->uuid[0] == '\0' ||
         !had_old_final ||
         strcmp(previous_profile->uuid, old_uuid) != 0 ||
         !read_connection_policy(previous_profile->uuid,
                                 target_autoconnect,
                                 sizeof(target_autoconnect),
                                 target_priority,
                                 sizeof(target_priority))))
    {
        printf("[失败] 无法确认原连接的自动连接策略，本次修改停止。\n");
        return false;
    }

    printf("\n========== 正在保存网络设置 ==========\n");

    /*
     * 安全顺序：
     * 1. 如果有旧正式 profile，先改名做备份，但暂时保留其 autoconnect 状态。
     * 2. 把新测试 profile 改成正式名称。
     * 3. 开启新 profile 的 autoconnect。
     * 4. 最后才关闭旧备份 profile 的 autoconnect。
     *
     * 这样即使步骤 2/3 失败，旧 profile 在重启后仍有机会自动恢复。
     */
    if (had_old_final)
    {
        make_backup_name(final_profile_name,
                         backup, sizeof(backup));

        printf("检测到旧的正式配置 \"%s\"。\n", final_profile_name);
        printf("旧配置会先保留一份，名称为：%s\n", backup);

        {
            const char *rename_old[] = {
                "nmcli", "connection", "modify",
                "uuid", old_uuid,
                "connection.id", backup,
                NULL};

            if (run_cmd(rename_old) != 0)
            {
                printf("[失败] 无法保留旧网络配置，本次保存停止。\n");
                return false;
            }
        }
    }

    {
        const char *rename_new[] = {
            "nmcli", "connection", "modify",
            "uuid", temp_profile->uuid,
            "connection.id", final_profile_name,
            NULL};

        if (run_cmd(rename_new) != 0)
        {
            printf("[失败] 无法保存新的网络配置。\n");

            if (had_old_final && backup[0] != '\0')
            {
                const char *restore_name[] = {
                    "nmcli", "connection", "modify",
                    "uuid", old_uuid,
                    "connection.id", final_profile_name,
                    NULL};
                printf("正在恢复旧网络配置...\n");
                run_cmd(restore_name);
            }

            return false;
        }
    }

    {
        const char *enable_new[] = {
            "nmcli", "connection", "modify",
            "uuid", temp_profile->uuid,
            "connection.autoconnect", target_autoconnect,
            "connection.autoconnect-priority", target_priority,
            NULL};

        if (run_cmd(enable_new) != 0)
        {
            printf("[失败] 无法保存新连接的自动连接策略。\n");
            printf("旧网络仍保留为开机可用状态，因此重启后仍有恢复机会。\n");
            return false;
        }
    }

    if (had_old_final && backup[0] != '\0')
    {
        const char *disable_backup[] = {
            "nmcli", "connection", "modify",
            "uuid", old_uuid,
            "connection.autoconnect", "no",
            NULL};

        if (run_cmd(disable_backup) != 0)
        {
            printf("[提醒] 新网络已经保存，但旧网络仍可能在开机时自动连接。\n");
            printf("建议进入“查看已保存的网络配置”检查新旧两项设置。\n");
        }
    }

    if (previous_profile && previous_profile->uuid[0] != '\0' &&
        strcmp(previous_profile->uuid, temp_profile->uuid) != 0 &&
        (!had_old_final || strcmp(previous_profile->uuid, old_uuid) != 0))
    {
        const char *disable_previous[] = {
            "nmcli", "connection", "modify", "uuid",
            previous_profile->uuid, "connection.autoconnect", "no", NULL};

        if (run_cmd(disable_previous) != 0)
        {
            printf("[提醒] 新连接已保存，但原连接仍可能自动连接。\n");
        }
        else
        {
            printf("[保留] 原连接仍在，但不再自动连接：%s\n",
                   previous_profile->name);
        }
    }

    printf("\n[完成] %s已经保存：%s\n",
           preserve_connection_policy ? "修改后的网络" : "新网络",
           final_profile_name);
    if (preserve_connection_policy)
    {
        printf("[完成] 已保留原来的开机自动连接设置和自动连接优先级\n");
    }
    else
    {
        printf("[完成] 新网络已设置为自动连接\n");
        printf("[完成] 新网络已设置为较高路由优先级\n");
    }

    if (had_old_final && backup[0] != '\0')
    {
        printf("[保留] 修改前的网络配置仍保存在：%s\n", backup);
    }

    return true;
}

bool finalize_profile(const ConnectionProfile *temp_profile,
                      const char *final_profile_name,
                      const ConnectionProfile *previous_profile)
{
    return finalize_profile_internal(temp_profile,
                                     final_profile_name,
                                     previous_profile,
                                     false);
}

bool finalize_edited_profile(const ConnectionProfile *temp_profile,
                             const char *final_profile_name,
                             const ConnectionProfile *original_profile)
{
    return finalize_profile_internal(temp_profile,
                                     final_profile_name,
                                     original_profile,
                                     true);
}

void cleanup_temp_profile(const ConnectionProfile *temp_profile)
{
    char resolved_uuid[UUID_SIZE] = {0};

    if (!resolve_connection_uuid(temp_profile->uuid,
                                 resolved_uuid,
                                 sizeof(resolved_uuid)))
    {
        return;
    }

    /*
     * 如果临时 profile 已经处于活动状态，不直接删除。
     * 否则可能把当前远程连接再次切断。
     * 由于临时 profile 的 autoconnect=no，重启后它不会自动抢占旧网络。
     */
    if (connection_is_active(temp_profile->uuid))
    {
        printf("\n[安全保护] 测试网络 %s 仍处于启用状态。\n",
               temp_profile->name);
        printf("为了避免再次断网，程序暂时不会删除它。\n");
        printf("它不会自动连接，重启后不会替代原网络。\n");
        return;
    }

    printf("\n正在删除本次失败的测试网络：%s\n", temp_profile->name);
    (void)delete_connection_by_uuid(temp_profile->uuid);
}

bool ask_final_profile_name(const char *default_name,
                                   char *out,
                                   size_t out_size)
{
    char prompt[256];
    char def[PROFILE_SIZE];

    snprintf(def, sizeof(def), "%s",
             default_name && default_name[0]
                 ? default_name
                 : "Network-Connection");

    snprintf(prompt, sizeof(prompt),
             "正式连接名称（直接回车默认 %s）: ",
             def);
    terminal_ui_set_step("设置连接名称",
                         "用于在系统中识别这项网络配置");

    for (;;)
    {
        bool valid = true;

        if (!read_line(prompt, out, out_size))
        {
            return false;
        }
        trim_space(out);
        if (out[0] == '\0')
        {
            snprintf(out, out_size, "%s", def);
            return true;
        }
        if (strlen(out) > 80)
        {
            valid = false;
        }
        for (const unsigned char *cursor = (const unsigned char *)out;
             valid && *cursor; ++cursor)
        {
            if (*cursor < 0x20 || *cursor == 0x7f)
            {
                valid = false;
            }
        }
        if (valid)
        {
            return true;
        }
        printf("连接名称不能为空、不能包含控制字符，并且最多 80 个字节。\n");
    }
}

bool build_temp_profile(const char *iface, ConnectionProfile *out)
{
    char nonce[UUID_SIZE];
    int written;

    if (!iface || iface[0] == '\0' || !out)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (!generate_connection_uuid(nonce, sizeof(nonce)))
    {
        return false;
    }

    /*
     * 不把这个 UUID 写入 connection.uuid。旧版 nmcli 会把它当作对不可变
     * 属性的修改并拒绝创建。这里只把随机值用于连接名称，确保之后可以无歧义
     * 地读取由 NetworkManager 自己生成的真实 UUID。
     */
    written = snprintf(out->name, sizeof(out->name),
                       "nettool-test-%s-%s", iface, nonce);
    if (written < 0 || (size_t)written >= sizeof(out->name))
    {
        memset(out, 0, sizeof(*out));
        return false;
    }

    return true;
}

bool bind_created_profile_uuid(ConnectionProfile *profile)
{
    char generated_uuid[UUID_SIZE] = {0};
    int matches;

    if (!profile || profile->name[0] == '\0')
    {
        return false;
    }

    matches = find_connection_uuid_by_name(profile->name,
                                           generated_uuid,
                                           sizeof(generated_uuid));
    if (matches == 1)
    {
        snprintf(profile->uuid, sizeof(profile->uuid), "%s", generated_uuid);
        return true;
    }

    profile->uuid[0] = '\0';
    if (matches > 1)
    {
        printf("[失败] 测试连接重名，无法确认内部标识：%s\n",
               profile->name);
        printf("        为避免误删其他连接，程序不会按名称自动清理。\n");
        return false;
    }

    if (matches == 0)
    {
        printf("[失败] 系统报告创建成功，但没有找到测试连接：%s\n",
               profile->name);
    }
    else
    {
        printf("[失败] 无法读取测试连接的内部标识：%s\n",
               profile->name);
    }

    /*
     * 名称包含本次生成的完整随机值。在 UUID 无法读取时，只能使用这个唯一名称
     * 回收可能已经创建的 profile，避免留下 autoconnect=no 的垃圾配置。
     */
    {
        const char *remove[] = {
            "nmcli", "connection", "delete", "id", profile->name, NULL};

        printf("正在按唯一名称清理未确认的测试连接...\n");
        if (run_cmd(remove) != 0)
        {
            printf("[提醒] 自动清理未成功，请检查是否残留连接：%s\n",
                   profile->name);
        }
    }

    return false;
}
