#define _POSIX_C_SOURCE 200809L

#include "network_safety.h"
#include "cli_io.h"
#include "command_runner.h"
#include "ipv4_utils.h"
#include "network_state.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool g_checkpoint_supported = false;

void network_safety_set_checkpoint_supported(bool supported)
{
    g_checkpoint_supported = supported;
}

static int ask_ipv4_mode(void)
{
    printf("\nIP 地址设置方式：\n");
    printf("  1. 自动获取 IP 地址\n");
    printf("  2. 手动填写固定 IP 地址\n");

    return read_int("请选择 [1-2]: ", 1, 2);
}

int configure_ipv4_profile(const char *profile_uuid, IPv4Config *cfg)
{
    int mode;

    memset(cfg, 0, sizeof(*cfg));
    mode = ask_ipv4_mode();

    if (mode == 1)
    {
        const char *argv[] = {
            "nmcli", "connection", "modify", "uuid", profile_uuid,
            "ipv4.method", "auto",
            "ipv4.addresses", "",
            "ipv4.gateway", "",
            "ipv4.dns", "",
            "ipv4.dad-timeout", "-1",
            NULL};

        cfg->is_static = false;
        return run_cmd(argv);
    }

    cfg->is_static = true;

    printf("\n固定 IP 是这台盒子在当前网络中的固定地址。\n");
    printf("如果现场给你的地址是 10.202.30.29，就原样填写 10.202.30.29。\n");

    for (;;)
    {
        read_line("IPv4 地址: ", cfg->ip, sizeof(cfg->ip));
        trim_space(cfg->ip);
        if (valid_ipv4(cfg->ip))
            break;
        printf("IP 地址格式不正确，例如：10.202.30.29\n");
    }

    printf("\n子网掩码用来判断哪些设备和这台盒子属于同一个局域网。\n");
    printf("如果现场给你的是 255.255.255.0，就原样填写。\n");
    printf("如果现场没有提供，请先向网络管理人员确认，不建议猜测。\n");

    for (;;)
    {
        char mask[INET_ADDRSTRLEN];
        read_line("子网掩码: ", mask, sizeof(mask));
        trim_space(mask);
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

    printf("\n网关可以理解为：盒子访问当前局域网以外的设备时，要先把数据交给的地址。\n");
    printf("如果现场给你的网关是 10.202.30.254，就原样填写。\n");
    printf("如果现场明确不需要网关，可以直接回车；如果不知道，请先确认。\n");

    for (;;)
    {
        read_line("默认网关（可留空）: ",
                  cfg->gateway, sizeof(cfg->gateway));
        trim_space(cfg->gateway);

        if (cfg->gateway[0] == '\0')
            break;
        if (!valid_ipv4(cfg->gateway))
        {
            printf("网关地址格式不正确，例如：10.202.30.254\n");
            continue;
        }
        if (!ipv4_same_subnet(cfg->ip, cfg->gateway, cfg->prefix))
        {
            printf("警告：网关 %s 和设备 IP %s/%d 看起来不在同一个局域网。\n",
                   cfg->gateway, cfg->ip, cfg->prefix);
            printf("这种情况通常是参数填写错误，请重新核对现场提供的信息。\n");
            continue;
        }
        break;
    }

    printf("\n域名服务器用于把网站名或服务器名称转换成 IP 地址。\n");
    printf("如果盒子只使用 IP 地址通信，可以直接回车留空。\n");
    read_line("DNS（可留空；多个地址用逗号分隔）: ",
              cfg->dns, sizeof(cfg->dns));
    trim_space(cfg->dns);

    {
        char cidr[32];
        snprintf(cidr, sizeof(cidr), "%s/%d", cfg->ip, cfg->prefix);
        const char *argv[] = {
            "nmcli", "connection", "modify", "uuid", profile_uuid,
            "ipv4.method", "manual",
            "ipv4.addresses", cidr,
            "ipv4.gateway", cfg->gateway,
            "ipv4.dns", cfg->dns,
            "ipv4.dad-timeout", IPV4_DAD_TIMEOUT_MS,
            NULL};
        if (run_cmd(argv) != 0)
            return -1;
    }

    printf("\n[安全检查] 已开启 IP 地址占用检查，最长等待 %s 毫秒。\n",
           IPV4_DAD_TIMEOUT_MS);
    printf("如果当前网络里已经有设备使用这个 IP，本次设置会停止，不会继续保存。\n");
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
        printf("[正常] 当前正在使用的新网络配置：%s\n", profile->name);
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
                printf("       当前仍在使用：%s\n", active_name);
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

        if (run_cmd(argv) == 0)
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

    if (fd > STDERR_FILENO)
    {
        close(fd);
    }
}

static void rollback_connection_now(const char *iface,
                                    const ConnectionProfile *new_profile,
                                    const ConnectionProfile *old_profile)
{
    printf("\n[正在恢复] 正在恢复修改前使用的网络...\n");

    if (new_profile && new_profile->uuid[0] != '\0')
    {
        const char *down[] = {
            "nmcli", "--wait", "15",
            "connection", "down", "uuid", new_profile->uuid,
            NULL};
        run_cmd_silent(down);
    }

    if (old_profile && old_profile->uuid[0] != '\0')
    {
        const char *up_old[] = {
            "nmcli", "--wait", "30",
            "connection", "up", "uuid", old_profile->uuid,
            "ifname", iface,
            NULL};

        if (run_cmd(up_old) == 0)
        {
            printf("[恢复完成] 已重新使用修改前的网络：%s\n", old_profile->name);
        }
        else
        {
            printf("[提醒] 自动恢复修改前的网络失败：%s\n", old_profile->name);
            printf("       如果还能通过 USB 或其他网口进入盒子，请查看网络状态并重新选择原来的网络。\n");
        }
    }
    else
    {
        printf("[提醒] 修改前这个网口没有正在使用的网络配置，因此没有可自动恢复的旧网络。\n");
    }
}

static pid_t start_rollback_watchdog(const char *iface,
                                     const ConnectionProfile *new_profile,
                                     const ConnectionProfile *old_profile)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork watchdog");
        return -1;
    }

    if (pid == 0)
    {
        /*
         * 脱离 SSH/当前终端。即使 SSH 因切网断开，
         * 这个子进程也应继续存活并在超时后回滚。
         */
        if (setsid() < 0)
        {
            _exit(120);
        }

        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        redirect_stdio_to_devnull();

        sleep(FALLBACK_ROLLBACK_TIMEOUT_SEC);

        if (new_profile && new_profile->uuid[0] != '\0')
        {
            const char *down[] = {
                "nmcli", "--wait", "15",
                "connection", "down", "uuid", new_profile->uuid,
                NULL};
            run_cmd_silent(down);
        }

        if (old_profile && old_profile->uuid[0] != '\0')
        {
            const char *up_old[] = {
                "nmcli", "--wait", "30",
                "connection", "up", "uuid", old_profile->uuid,
                "ifname", iface,
                NULL};
            run_cmd_silent(up_old);
        }

        _exit(0);
    }

    return pid;
}

static void cancel_rollback_watchdog(pid_t watchdog_pid)
{
    if (watchdog_pid <= 0)
    {
        return;
    }

    if (kill(watchdog_pid, SIGTERM) == 0)
    {
        (void)waitpid(watchdog_pid, NULL, 0);
    }
    else if (errno == ESRCH)
    {
        (void)waitpid(watchdog_pid, NULL, WNOHANG);
    }
}

static bool safe_activate_with_fallback_watchdog(const char *iface,
                                                 const ConnectionProfile *profile,
                                                 const IPv4Config *cfg)
{
    ConnectionProfile old_profile = {0};
    pid_t watchdog_pid;

    if (get_active_connection_uuid(iface,
                                   old_profile.uuid,
                                   sizeof(old_profile.uuid)))
    {
        (void)get_connection_name(old_profile.uuid,
                                  old_profile.name,
                                  sizeof(old_profile.name));
    }

    printf("\n");
    printf("============================================================\n");
    printf("                 开始前请先了解这一步\n");
    printf("============================================================\n");
    printf("程序不会马上把新网络永久保存，而是先临时测试。\n");
    printf("\n");
    if (old_profile.uuid[0] != '\0')
    {
        printf("修改前，%s 正在使用：%s\n", iface, old_profile.name);
    }
    else
    {
        printf("修改前，%s 没有正在使用的已保存网络。\n", iface);
    }
    printf("\n");
    printf("接下来会发生以下事情：\n");
    printf("  1. 程序启动自动恢复保护。\n");
    printf("  2. 临时切换到你刚刚填写的新网络。\n");
    printf("  3. 如果新网络正常，程序会让你输入 YES 确认。\n");
    printf("  4. 如果 SSH 因换网而断开，请不要重启盒子，也不要继续修改网络。\n");
    printf("  5. 等待最多 %d 秒，程序会尝试重新启用修改前的网络。\n",
           FALLBACK_ROLLBACK_TIMEOUT_SEC);
    printf("\n");
    printf("只有你明确输入 YES 后，新网络才会进入后续保存步骤。\n");
    printf("============================================================\n");

    if (!read_yes_no("现在开始测试新网络吗？[Y/n]: ", true))
    {
        return false;
    }

    watchdog_pid = start_rollback_watchdog(iface,
                                           profile,
                                           &old_profile);
    if (watchdog_pid < 0)
    {
        printf("[失败] 无法启动自动恢复保护。为了避免设备失联，本次操作已取消。\n");
        return false;
    }

    printf("[安全保护] 自动恢复保护已经启动，进程编号为 %ld。\n",
           (long)watchdog_pid);

    {
        const char *up_new[] = {
            "nmcli", "--wait", NMCLI_UP_WAIT_SEC,
            "connection", "up", "uuid", profile->uuid,
            "ifname", iface,
            NULL};

        if (run_cmd(up_new) != 0)
        {
            printf("\n[失败] 新网络没有成功启用，正在恢复修改前的网络。\n");
            rollback_connection_now(iface,
                                    profile,
                                    &old_profile);
            cancel_rollback_watchdog(watchdog_pid);
            return false;
        }
    }

    if (!validate_new_connection(iface, profile, cfg))
    {
        printf("\n[失败] 新网络检查没有通过，正在恢复修改前的网络。\n");
        rollback_connection_now(iface,
                                profile,
                                &old_profile);
        cancel_rollback_watchdog(watchdog_pid);
        return false;
    }

    optional_gateway_ping(cfg);

    printf("\n");
    printf("============================================================\n");
    printf("                 新网络基本检查通过\n");
    printf("============================================================\n");
    printf("现在请你做最后确认：\n");
    printf("  - 如果你仍然可以正常操作盒子，说明新网络至少没有让当前管理连接失效。\n");
    printf("  - 如果你是通过 SSH 操作，而 SSH 已经断开，请不要做任何事。\n");
    printf("    等待自动恢复保护把网络恢复到修改前的状态。\n");
    printf("\n");
    printf("确认新网络正常后，输入 YES 才会继续保存。\n");
    printf("============================================================\n");

    if (!read_exact_yes("确认新网络可以正常使用请输入 YES；其他输入将恢复原网络: "))
    {
        printf("\n没有收到明确的 YES，程序将恢复修改前的网络。\n");
        rollback_connection_now(iface,
                                profile,
                                &old_profile);
        cancel_rollback_watchdog(watchdog_pid);
        return false;
    }

    cancel_rollback_watchdog(watchdog_pid);
    printf("[安全保护] 已确认新网络正常，自动恢复保护已停止。\n");

    return true;
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

static bool run_checkpoint_activation(const char *iface,
                                      const ConnectionProfile *profile,
                                      const IPv4Config *cfg)
{
    int input_pipe[2];
    pid_t pid;
    int status = 0;
    bool state_ready = false;
    bool confirmed = false;
    const char *decision = "No\n";

    if (pipe(input_pipe) != 0)
    {
        perror("pipe checkpoint");
        return false;
    }

    pid = fork();
    if (pid < 0)
    {
        perror("fork checkpoint");
        close(input_pipe[0]);
        close(input_pipe[1]);
        return false;
    }

    if (pid == 0)
    {
        const char *argv[] = {
            "nmcli",
            "device",
            "checkpoint",
            "--timeout", CHECKPOINT_TIMEOUT_SEC,
            iface,
            "--",
            "nmcli",
            "--wait", NMCLI_UP_WAIT_SEC,
            "connection",
            "up",
            "uuid", profile->uuid,
            "ifname", iface,
            NULL};

        close(input_pipe[1]);
        if (dup2(input_pipe[0], STDIN_FILENO) < 0)
        {
            _exit(126);
        }
        close(input_pipe[0]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(input_pipe[0]);

    /*
     * nmcli 此时持有 NetworkManager checkpoint，并在内部命令执行后
     * 等待 Yes/No。父进程先独立检查 UUID、IP 和网关，再决定是否提交。
     * 如果父进程或 SSH 意外退出，管道关闭且 checkpoint 超时，
     * NetworkManager 会恢复创建 checkpoint 时的网络状态。
     */
    for (int elapsed_ms = 0; elapsed_ms < 60000; elapsed_ms += 500)
    {
        struct timespec delay = {0, 500L * 1000L * 1000L};
        pid_t wait_result = waitpid(pid, &status, WNOHANG);

        if (wait_result == pid)
        {
            close(input_pipe[1]);
            return false;
        }

        if (connection_state_matches(iface, profile, cfg))
        {
            state_ready = true;
            break;
        }

        nanosleep(&delay, NULL);
    }

    if (state_ready && validate_new_connection(iface, profile, cfg))
    {
        optional_gateway_ping(cfg);

        printf("\n============================================================\n");
        printf("新网络检查已经通过，但 checkpoint 还没有提交。\n");
        printf("只有明确输入 YES，NetworkManager 才会保留当前切换。\n");
        printf("如果 SSH 已经断开，不需要操作；超时后会自动恢复旧网络。\n");
        printf("============================================================\n");

        confirmed = read_exact_yes(
            "确认新网络可以正常使用请输入 YES；其他输入将恢复原网络: ");
    }

    if (confirmed)
    {
        decision = "Yes\n";
    }
    else
    {
        printf("\n新网络未通过完整检查或未收到明确确认，正在请求恢复旧网络。\n");
    }

    signal(SIGPIPE, SIG_IGN);
    (void)write(input_pipe[1], decision, strlen(decision));
    close(input_pipe[1]);

    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }

    if (!confirmed || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return false;
    }

    return validate_new_connection(iface, profile, cfg);
}

bool safe_activate_with_checkpoint(const char *iface,
                                   const ConnectionProfile *profile,
                                   const IPv4Config *cfg)
{
    if (!g_checkpoint_supported)
    {
        return safe_activate_with_fallback_watchdog(iface,
                                                    profile,
                                                    cfg);
    }

    printf("\n");
    printf("============================================================\n");
    printf("接下来会临时切换到新网络进行测试。\n");
    printf("系统会先保存当前网络状态，然后再尝试使用新网络。\n");
    printf("如果切换后 SSH 断开并且无法确认，等待一段时间后系统会自动恢复修改前的网络。\n");
    printf("测试用的新网络暂时不会设置为开机自动使用，因此测试失败不会影响下次启动。\n");
    printf("\n");
    printf("程序会先完成 UUID、IP 和网关检查，再要求你输入 YES。\n");
    printf("只有检查通过并明确确认后才会提交；否则恢复修改前的网络。\n");
    printf("============================================================\n");

    if (!read_yes_no("现在开始测试新网络吗？[Y/n]: ", true))
    {
        return false;
    }

    {
        bool ok = run_checkpoint_activation(iface, profile, cfg);

        if (!ok)
        {
            printf("\n[失败] 新网络没有通过检查或没有被确认。\n");
            printf("NetworkManager 将恢复 checkpoint 创建前的网络状态。\n");
        }

        return ok;
    }
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

bool finalize_profile(const ConnectionProfile *temp_profile,
                      const char *final_profile_name)
{
    char backup[PROFILE_SIZE] = {0};
    char old_uuid[UUID_SIZE] = {0};
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
        printf("为避免修改错误的连接，请先按 UUID 清理重名项。\n");
        return false;
    }

    had_old_final = final_name_matches == 1 &&
                    strcmp(old_uuid, temp_profile->uuid) != 0;

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
            "connection.autoconnect", "yes",
            "connection.autoconnect-priority", "100",
            NULL};

        if (run_cmd(enable_new) != 0)
        {
            printf("[失败] 无法把新网络设置为开机自动使用。\n");
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

    printf("\n[完成] 新网络已经保存：%s\n", final_profile_name);
    printf("[完成] 新网络已设置为开机自动使用\n");
    printf("[完成] 新网络已设置为较高使用优先级\n");

    if (had_old_final && backup[0] != '\0')
    {
        printf("[保留] 修改前的网络配置仍保存在：%s\n", backup);
    }

    return true;
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
        printf("\n[安全保护] 测试用网络 %s 当前仍在使用。\n",
               temp_profile->name);
        printf("为了避免再次断网，程序暂时不会删除它。\n");
        printf("它没有设置为开机自动使用，重启后不会自动替代原来的网络。\n");
        return;
    }

    printf("\n正在删除本次失败的测试网络：%s\n", temp_profile->name);
    (void)delete_connection_by_uuid(temp_profile->uuid);
}

void ask_final_profile_name(const char *iface,
                                   char *out,
                                   size_t out_size)
{
    char prompt[256];
    char def[PROFILE_SIZE];

    snprintf(def, sizeof(def), "nettool-%s", iface);

    snprintf(prompt, sizeof(prompt),
             "正式连接名称（直接回车默认 %s）: ",
             def);

    read_line(prompt, out, out_size);
    trim_space(out);

    if (out[0] == '\0')
    {
        snprintf(out, out_size, "%s", def);
    }
}

bool build_temp_profile(const char *iface, ConnectionProfile *out)
{
    if (!out || !generate_connection_uuid(out->uuid, sizeof(out->uuid)))
    {
        return false;
    }

    snprintf(out->name, sizeof(out->name),
             "nettool-test-%s-%ld",
             iface, (long)getpid());
    return true;
}
