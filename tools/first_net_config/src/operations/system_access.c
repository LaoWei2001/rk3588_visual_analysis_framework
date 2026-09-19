#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "operations/system_access.h"

#include "common/cli_io.h"
#include "common/command_runner.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ACCOUNT_NAME_SIZE 33
#define DISPLAY_ACCOUNT_NAME_SIZE 256
#define ACCOUNT_PASSWORD_SIZE 129
#define MAX_LOGIN_USERS 128
#define SSHD_CONFIG_PATH "/etc/ssh/sshd_config"
#define SSHD_DROPIN_DIR "/etc/ssh/sshd_config.d"
#define SSHD_DROPIN_PATH SSHD_DROPIN_DIR "/000-first-net-config.conf"

typedef enum
{
    PASSWORD_UNKNOWN = 0,
    PASSWORD_EMPTY,
    PASSWORD_LOCKED,
    PASSWORD_SET
} PasswordState;

typedef struct
{
    char *data;
    size_t size;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    bool existed;
} FileSnapshot;

static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *cursor = memory;

    while (cursor && size-- > 0)
    {
        *cursor++ = 0;
    }
}

static PasswordState account_password_state(const char *username)
{
    struct spwd *shadow;
    const char *password;

    errno = 0;
    shadow = username ? getspnam(username) : NULL;
    if (!shadow || !(password = shadow->sp_pwdp))
    {
        return PASSWORD_UNKNOWN;
    }
    if (password[0] == '\0')
    {
        return PASSWORD_EMPTY;
    }
    if (password[0] == '!' || password[0] == '*')
    {
        return PASSWORD_LOCKED;
    }
    return PASSWORD_SET;
}

static const char *password_state_text(PasswordState state)
{
    switch (state)
    {
    case PASSWORD_EMPTY:
        return "未设置";
    case PASSWORD_LOCKED:
        return "已锁定";
    case PASSWORD_SET:
        return "已设置";
    case PASSWORD_UNKNOWN:
    default:
        return "无法读取";
    }
}

static bool password_is_usable(PasswordState state)
{
    return state == PASSWORD_SET;
}

static bool valid_account_password(const char *password)
{
    size_t length = password ? strlen(password) : 0;

    if (length == 0 || length >= ACCOUNT_PASSWORD_SIZE)
    {
        return false;
    }
    for (size_t index = 0; index < length; ++index)
    {
        unsigned char character = (unsigned char)password[index];
        if (character == '\n' || character == '\r' || character == '\0')
        {
            return false;
        }
    }
    return true;
}

static bool read_confirmed_password(char *password, size_t password_size)
{
    char confirmation[ACCOUNT_PASSWORD_SIZE];

    if (!password || password_size < ACCOUNT_PASSWORD_SIZE)
    {
        return false;
    }
    for (;;)
    {
        terminal_ui_prepare_step(true);
        if (!read_password("请输入新密码（1～128个字符）: ",
                           password, password_size))
        {
            secure_clear(password, password_size);
            return false;
        }
        if (!valid_account_password(password))
        {
            printf("密码不能为空，最长128个字符。\n");
            secure_clear(password, password_size);
            continue;
        }
        if (!read_password("请再次输入新密码: ",
                           confirmation, sizeof(confirmation)))
        {
            secure_clear(password, password_size);
            secure_clear(confirmation, sizeof(confirmation));
            return false;
        }
        if (strcmp(password, confirmation) == 0)
        {
            secure_clear(confirmation, sizeof(confirmation));
            return true;
        }
        printf("两次密码不一致，请重新输入。\n");
        secure_clear(password, password_size);
        secure_clear(confirmation, sizeof(confirmation));
    }

}

static bool set_account_password(const char *username, const char *password)
{
    char input[ACCOUNT_NAME_SIZE + ACCOUNT_PASSWORD_SIZE + 3];
    int length;
    int result;
    const char *argv[] = {"chpasswd", NULL};

    if (!username || !password)
    {
        return false;
    }
    length = snprintf(input, sizeof(input), "%s:%s\n", username, password);
    if (length < 0 || (size_t)length >= sizeof(input))
    {
        secure_clear(input, sizeof(input));
        return false;
    }
    result = run_cmd_with_input_silent(argv, input, (size_t)length);
    secure_clear(input, sizeof(input));
    return result == 0;
}

static void set_root_password(void)
{
    char password[ACCOUNT_PASSWORD_SIZE];
    PasswordState state = account_password_state("root");

    printf("\n当前 root 密码状态：%s\n", password_state_text(state));
    terminal_ui_set_step("设置 root 密码",
                         "密码仅显示星号，不会进入命令参数或日志");
    if (!read_confirmed_password(password, sizeof(password)))
    {
        printf("已取消设置 root 密码。\n");
        return;
    }
    terminal_ui_set_step("确认 root 密码修改",
                         "保存后旧密码立即失效");
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定设置或修改 root 密码吗？[y/N]: ", false))
    {
        secure_clear(password, sizeof(password));
        printf("已取消。\n");
        return;
    }
    if (set_account_password("root", password))
    {
        printf("[完成] root 密码已设置，账号密码锁定状态已解除。\n");
    }
    else
    {
        printf("[失败] 无法设置 root 密码，请确认系统提供 chpasswd 和 PAM 密码服务。\n");
    }
    secure_clear(password, sizeof(password));
}

static void free_snapshot(FileSnapshot *snapshot)
{
    if (!snapshot)
    {
        return;
    }
    free(snapshot->data);
    memset(snapshot, 0, sizeof(*snapshot));
}

static bool take_snapshot(const char *path, FileSnapshot *snapshot)
{
    struct stat info;
    FILE *file;
    long length;

    if (!path || !snapshot)
    {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (stat(path, &info) != 0)
    {
        return errno == ENOENT;
    }
    snapshot->existed = true;
    snapshot->mode = info.st_mode & 07777;
    snapshot->uid = info.st_uid;
    snapshot->gid = info.st_gid;
    file = fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }
    snapshot->data = malloc((size_t)length + 1);
    if (!snapshot->data)
    {
        fclose(file);
        return false;
    }
    snapshot->size = fread(snapshot->data, 1, (size_t)length, file);
    if (snapshot->size != (size_t)length || ferror(file))
    {
        fclose(file);
        free_snapshot(snapshot);
        return false;
    }
    snapshot->data[snapshot->size] = '\0';
    fclose(file);
    return true;
}

static bool write_all(int descriptor, const char *data, size_t size)
{
    size_t written = 0;

    while (written < size)
    {
        ssize_t count = write(descriptor, data + written, size - written);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        written += (size_t)count;
    }
    return true;
}

static bool atomic_write_file(const char *path, const char *temporary_pattern,
                              const char *data, size_t size,
                              mode_t mode, uid_t uid, gid_t gid)
{
    char temporary[256];
    int descriptor;
    bool ok;

    if (!path || !temporary_pattern || !data ||
        snprintf(temporary, sizeof(temporary), "%s", temporary_pattern) < 0)
    {
        return false;
    }
    descriptor = mkstemp(temporary);
    if (descriptor < 0)
    {
        return false;
    }
    ok = fchmod(descriptor, mode) == 0 &&
         fchown(descriptor, uid, gid) == 0 &&
         write_all(descriptor, data, size) && fsync(descriptor) == 0;
    if (close(descriptor) != 0)
    {
        ok = false;
    }
    if (ok)
    {
        ok = rename(temporary, path) == 0;
    }
    if (!ok)
    {
        unlink(temporary);
    }
    return ok;
}

static bool restore_snapshot(const char *path, const char *temporary_pattern,
                             const FileSnapshot *snapshot)
{
    if (!snapshot)
    {
        return false;
    }
    if (!snapshot->existed)
    {
        return unlink(path) == 0 || errno == ENOENT;
    }
    return atomic_write_file(path, temporary_pattern,
                             snapshot->data, snapshot->size,
                             snapshot->mode, snapshot->uid, snapshot->gid);
}

static bool first_sshd_directive_is_managed_include(const char *content)
{
    const char *line = content;

    while (line && *line)
    {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        const char *cursor = line;

        while ((size_t)(cursor - line) < length && isspace((unsigned char)*cursor))
        {
            ++cursor;
        }
        if ((size_t)(cursor - line) < length && *cursor != '#')
        {
            static const char managed[] =
                "Include /etc/ssh/sshd_config.d/000-first-net-config.conf";
            size_t directive_length = length - (size_t)(cursor - line);

            while (directive_length > 0 &&
                   isspace((unsigned char)cursor[directive_length - 1]))
                --directive_length;
            return directive_length == sizeof(managed) - 1 &&
                   strncasecmp(cursor, managed, sizeof(managed) - 1) == 0;
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

static const char *find_sshd_executable(void)
{
    static const char *const candidates[] = {
        "/usr/sbin/sshd", "/usr/local/sbin/sshd", "/usr/bin/sshd", NULL};

    for (int index = 0; candidates[index]; ++index)
    {
        if (access(candidates[index], X_OK) == 0)
        {
            return candidates[index];
        }
    }
    return NULL;
}

static bool validate_sshd_config(const char *sshd)
{
    const char *argv[] = {sshd, "-t", "-f", SSHD_CONFIG_PATH, NULL};
    return sshd && run_cmd_silent(argv) == 0;
}

static bool start_or_reload_ssh_service(void)
{
    static const char *const units[] = {"ssh.service", "sshd.service", NULL};

    if (access("/run/systemd/system", F_OK) == 0)
    {
        for (int index = 0; units[index]; ++index)
        {
            const char *enable[] = {
                "systemctl", "enable", "--now", units[index], NULL};
            const char *reload[] = {
                "systemctl", "reload", units[index], NULL};
            const char *active[] = {
                "systemctl", "is-active", "--quiet", units[index], NULL};

            if (run_cmd_silent(enable) == 0)
            {
                (void)run_cmd_silent(reload);
                if (run_cmd_silent(active) == 0)
                {
                    return true;
                }
            }
        }
    }
    {
        const char *restart[] = {"service", "ssh", "restart", NULL};
        return run_cmd_silent(restart) == 0;
    }
}

static bool ssh_service_active(void)
{
    static const char *const units[] = {"ssh.service", "sshd.service", NULL};

    if (access("/run/systemd/system", F_OK) != 0)
    {
        return false;
    }
    for (int index = 0; units[index]; ++index)
    {
        const char *active[] = {
            "systemctl", "is-active", "--quiet", units[index], NULL};
        if (run_cmd_silent(active) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool effective_sshd_value(const char *key, char *value, size_t value_size)
{
    char output[32768];
    const char *sshd = find_sshd_executable();
    const char *argv[] = {
        sshd, "-T", "-f", SSHD_CONFIG_PATH,
        "-C", "user=root,host=localhost,addr=127.0.0.1", NULL};
    char *line;
    char *save = NULL;
    size_t key_length;

    if (!sshd || !key || !value || value_size == 0 ||
        capture_cmd(argv, output, sizeof(output)) != 0)
    {
        return false;
    }
    key_length = strlen(key);
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
    {
        if (strncasecmp(line, key, key_length) == 0 &&
            isspace((unsigned char)line[key_length]))
        {
            const char *source = line + key_length;
            while (*source && isspace((unsigned char)*source))
            {
                ++source;
            }
            snprintf(value, value_size, "%s", source);
            return true;
        }
    }
    return false;
}

static void show_ssh_status(void)
{
    char root_login[64] = "无法读取";
    char password_auth[64] = "无法读取";
    const char *sshd = find_sshd_executable();

    if (sshd)
    {
        (void)effective_sshd_value("permitrootlogin",
                                   root_login, sizeof(root_login));
        (void)effective_sshd_value("passwordauthentication",
                                   password_auth, sizeof(password_auth));
    }
    printf("\n========== SSH 与 root 状态 ==========\n");
    printf("SSH 服务程序       : %s\n", sshd ? sshd : "未安装或未找到");
    printf("SSH 服务运行状态   : %s\n", ssh_service_active() ? "运行中" : "未运行/无法确认");
    printf("root SSH 策略      : %s\n", root_login);
    printf("SSH 密码认证       : %s\n", password_auth);
    printf("root 密码状态      : %s\n",
           password_state_text(account_password_state("root")));
    printf("工具管理的配置     : %s\n",
           access(SSHD_DROPIN_PATH, F_OK) == 0 ? SSHD_DROPIN_PATH : "尚未创建");
}

static bool install_root_ssh_config(void)
{
    static const char dropin[] =
        "# Managed by first_net_config.\n"
        "# Password authentication is enabled for field maintenance accounts.\n"
        "PermitRootLogin yes\n"
        "PasswordAuthentication yes\n";
    static const char include_line[] =
        "Include /etc/ssh/sshd_config.d/000-first-net-config.conf\n";
    FileSnapshot main_config = {0};
    FileSnapshot dropin_config = {0};
    const char *sshd = find_sshd_executable();
    char *new_main = NULL;
    bool main_changed = false;
    bool ok = false;

    if (!sshd)
    {
        printf("[失败] 未找到 sshd，请先在系统镜像中安装 openssh-server。\n");
        return false;
    }
    if (!take_snapshot(SSHD_CONFIG_PATH, &main_config) ||
        !main_config.existed || !take_snapshot(SSHD_DROPIN_PATH, &dropin_config))
    {
        printf("[失败] 无法读取现有 SSH 配置，未进行修改。\n");
        free_snapshot(&main_config);
        free_snapshot(&dropin_config);
        return false;
    }
    if (mkdir(SSHD_DROPIN_DIR, 0755) != 0 && errno != EEXIST)
    {
        printf("[失败] 无法创建 %s。\n", SSHD_DROPIN_DIR);
        goto cleanup;
    }
    if (!first_sshd_directive_is_managed_include(main_config.data))
    {
        size_t new_size = sizeof(include_line) - 1 + main_config.size;
        new_main = malloc(new_size + 1);
        if (!new_main)
        {
            printf("[失败] 内存不足，未修改 SSH 配置。\n");
            goto cleanup;
        }
        memcpy(new_main, include_line, sizeof(include_line) - 1);
        memcpy(new_main + sizeof(include_line) - 1,
               main_config.data, main_config.size);
        new_main[new_size] = '\0';
        if (!atomic_write_file(
                SSHD_CONFIG_PATH,
                "/etc/ssh/.sshd_config.first_net_config.XXXXXX",
                new_main, new_size,
                main_config.mode, main_config.uid, main_config.gid))
        {
            printf("[失败] 无法为 sshd_config 启用配置片段目录。\n");
            goto cleanup;
        }
        main_changed = true;
    }
    if (!atomic_write_file(
            SSHD_DROPIN_PATH,
            SSHD_DROPIN_DIR "/.000-first-net-config.XXXXXX",
            dropin, sizeof(dropin) - 1, 0600, 0, 0))
    {
        printf("[失败] 无法写入 root SSH 配置。\n");
        if (main_changed)
        {
            (void)restore_snapshot(
                SSHD_CONFIG_PATH,
                "/etc/ssh/.sshd_config.first_net_config.XXXXXX",
                &main_config);
        }
        goto cleanup;
    }
    if (!validate_sshd_config(sshd))
    {
        printf("[失败] sshd 配置校验未通过，正在恢复修改前的配置。\n");
        (void)restore_snapshot(
            SSHD_DROPIN_PATH,
            SSHD_DROPIN_DIR "/.000-first-net-config.XXXXXX",
            &dropin_config);
        if (main_changed)
        {
            (void)restore_snapshot(
                SSHD_CONFIG_PATH,
                "/etc/ssh/.sshd_config.first_net_config.XXXXXX",
                &main_config);
        }
        goto cleanup;
    }
    if (!start_or_reload_ssh_service())
    {
        printf("[失败] SSH 服务无法启动或重载，正在恢复修改前的配置。\n");
        (void)restore_snapshot(
            SSHD_DROPIN_PATH,
            SSHD_DROPIN_DIR "/.000-first-net-config.XXXXXX",
            &dropin_config);
        if (main_changed)
        {
            (void)restore_snapshot(
                SSHD_CONFIG_PATH,
                "/etc/ssh/.sshd_config.first_net_config.XXXXXX",
                &main_config);
        }
        (void)start_or_reload_ssh_service();
        printf("请检查 systemctl status ssh.service。\n");
        goto cleanup;
    }
    {
        char root_login[64] = {0};
        char password_auth[64] = {0};
        if (!effective_sshd_value("permitrootlogin", root_login, sizeof(root_login)) ||
            !effective_sshd_value("passwordauthentication", password_auth,
                                  sizeof(password_auth)) ||
            strcasecmp(root_login, "yes") != 0 ||
            strcasecmp(password_auth, "yes") != 0)
        {
            printf("[失败] SSH 服务已重载，但生效配置仍未允许 root 密码登录。\n");
            printf("正在恢复修改前的 SSH 配置。\n");
            (void)restore_snapshot(
                SSHD_DROPIN_PATH,
                SSHD_DROPIN_DIR "/.000-first-net-config.XXXXXX",
                &dropin_config);
            if (main_changed)
            {
                (void)restore_snapshot(
                    SSHD_CONFIG_PATH,
                    "/etc/ssh/.sshd_config.first_net_config.XXXXXX",
                    &main_config);
            }
            (void)start_or_reload_ssh_service();
            printf("请检查 sshd -T 输出和系统的额外 SSH 策略。\n");
            goto cleanup;
        }
    }
    ok = true;

cleanup:
    free(new_main);
    free_snapshot(&main_config);
    free_snapshot(&dropin_config);
    return ok;
}

static void enable_root_ssh(void)
{
    PasswordState root_password;

    show_ssh_status();
    printf("\n[高风险提醒]\n");
    printf("此操作会允许 root 通过 SSH 登录，并开启 SSH 密码认证。\n");
    printf("设备可被不可信网络访问时，弱密码会带来严重安全风险。\n");
    terminal_ui_set_step("确认启用 root SSH",
                         "这是高风险系统配置，必须输入完整确认词");
    terminal_ui_prepare_step(true);
    if (!read_exact_word("确认请输入 ENABLE ROOT SSH；其他输入取消: ",
                         "ENABLE ROOT SSH"))
    {
        printf("已取消，SSH 配置没有修改。\n");
        return;
    }
    terminal_ui_show_busy("正在配置 SSH",
                          "写入配置、执行 sshd 校验并启动服务");
    if (!install_root_ssh_config())
    {
        return;
    }
    printf("[完成] 已允许 root SSH 登录并启用密码认证。\n");
    show_ssh_status();

    root_password = account_password_state("root");
    if (!password_is_usable(root_password))
    {
        printf("\n[需要处理] root 当前没有可用密码，暂时不能使用密码远程登录。\n");
        terminal_ui_set_step("设置 root 登录密码",
                             "SSH 已启用，建议立即设置强密码");
        terminal_ui_prepare_step(true);
        if (read_yes_no("现在设置 root 密码吗？[Y/n]: ", true))
        {
            set_root_password();
        }
    }
}

static bool valid_username(const char *username)
{
    size_t length = username ? strlen(username) : 0;

    if (length == 0 || length >= ACCOUNT_NAME_SIZE ||
        !(islower((unsigned char)username[0]) || username[0] == '_'))
    {
        return false;
    }
    for (size_t index = 1; index < length; ++index)
    {
        unsigned char character = (unsigned char)username[index];
        if (!islower(character) && !isdigit(character) &&
            character != '_' && character != '-')
        {
            return false;
        }
    }
    return true;
}

static void create_login_user(void)
{
    char username[ACCOUNT_NAME_SIZE];
    char password[ACCOUNT_PASSWORD_SIZE];
    char home[sizeof("/home/") + ACCOUNT_NAME_SIZE];
    const char *shell = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";

    terminal_ui_set_step("新建登录用户",
                         "创建 home 目录和登录 shell，不自动授予 sudo");
    for (;;)
    {
        terminal_ui_prepare_step(true);
        if (!read_line("新用户名（小写字母、数字、_、-，直接回车取消）: ",
                       username, sizeof(username)))
        {
            return;
        }
        trim_space(username);
        if (username[0] == '\0')
        {
            printf("已取消。\n");
            return;
        }
        if (!valid_username(username))
        {
            printf("用户名必须以小写字母或下划线开头，只能包含小写字母、数字、下划线和短横线，最长32个字符。\n");
            continue;
        }
        if (getpwnam(username))
        {
            printf("用户名“%s”已经存在，请换一个名称。\n", username);
            continue;
        }
        snprintf(home, sizeof(home), "/home/%s", username);
        if (access(home, F_OK) == 0)
        {
            printf("目录“%s”已经存在。为避免覆盖旧数据，请换一个用户名或先人工处理该目录。\n",
                   home);
            continue;
        }
        break;
    }

    terminal_ui_set_step("设置新用户密码",
                         "密码不会出现在进程参数或日志中");
    if (!read_confirmed_password(password, sizeof(password)))
    {
        printf("已取消，新用户尚未创建。\n");
        return;
    }
    printf("\n准备创建用户：%s\n", username);
    printf("home 目录     ：/home/%s\n", username);
    printf("登录 shell    ：%s\n", shell);
    printf("管理员权限    ：不授予\n");
    terminal_ui_set_step("确认创建用户",
                         "用户创建后可使用本机终端；SSH 还需服务允许密码认证");
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定创建吗？[y/N]: ", false))
    {
        secure_clear(password, sizeof(password));
        printf("已取消。\n");
        return;
    }
    {
        const char *add[] = {
            "useradd", "--create-home", "--user-group",
            "--shell", shell, "--", username, NULL};
        if (run_cmd_silent(add) != 0)
        {
            secure_clear(password, sizeof(password));
            printf("[失败] 无法创建用户，请确认系统提供 useradd，并检查系统日志。\n");
            return;
        }
    }
    if (!set_account_password(username, password))
    {
        const char *rollback[] = {"userdel", "--remove", "--", username, NULL};
        (void)run_cmd_silent(rollback);
        secure_clear(password, sizeof(password));
        printf("[失败] 用户密码设置失败，已尝试删除刚创建的账号。\n");
        return;
    }
    secure_clear(password, sizeof(password));
    printf("[完成] 用户“%s”已创建，并已设置密码。\n", username);
    printf("该用户未加入 sudo 等管理员组。\n");
    if (!ssh_service_active())
    {
        printf("[提醒] SSH 服务当前未运行；如需远程登录，请先启用 SSH。\n");
    }
}

static bool account_can_login(const char *shell)
{
    char *allowed;
    bool found = false;

    if (!shell || !shell[0] || strcmp(shell, "/bin/false") == 0 ||
        strstr(shell, "nologin") != NULL)
    {
        return false;
    }
    setusershell();
    while ((allowed = getusershell()) != NULL)
    {
        if (strcmp(shell, allowed) == 0)
        {
            found = true;
            break;
        }
    }
    endusershell();
    return found;
}

static void login_uid_range(uid_t *minimum, uid_t *maximum)
{
    FILE *file;
    char line[256];

    if (!minimum || !maximum)
    {
        return;
    }
    *minimum = 1000;
    *maximum = 60000;
    file = fopen("/etc/login.defs", "r");
    if (!file)
    {
        return;
    }
    while (fgets(line, sizeof(line), file))
    {
        char key[32];
        unsigned long value;

        if (line[0] == '#' || sscanf(line, "%31s %lu", key, &value) != 2)
        {
            continue;
        }
        if (strcmp(key, "UID_MIN") == 0)
        {
            *minimum = (uid_t)value;
        }
        else if (strcmp(key, "UID_MAX") == 0)
        {
            *maximum = (uid_t)value;
        }
    }
    fclose(file);
}

typedef struct
{
    char username[DISPLAY_ACCOUNT_NAME_SIZE];
    uid_t uid;
} LoginUser;

static int collect_ordinary_login_users(LoginUser *users, int capacity)
{
    struct passwd *entry;
    uid_t uid_min;
    uid_t uid_max;
    int count = 0;

    if (!users || capacity <= 0)
    {
        return 0;
    }
    login_uid_range(&uid_min, &uid_max);
    setpwent();
    while (count < capacity && (entry = getpwent()) != NULL)
    {
        if (entry->pw_uid < uid_min || entry->pw_uid > uid_max ||
            !account_can_login(entry->pw_shell))
        {
            continue;
        }
        snprintf(users[count].username, sizeof(users[count].username),
                 "%s", entry->pw_name);
        users[count].uid = entry->pw_uid;
        ++count;
    }
    endpwent();
    return count;
}

static void modify_login_user_password(void)
{
    LoginUser users[MAX_LOGIN_USERS];
    char password[ACCOUNT_PASSWORD_SIZE];
    int count = collect_ordinary_login_users(users, MAX_LOGIN_USERS);
    int selected;
    const char *username;

    if (count == 0)
    {
        printf("目前没有可以修改密码的普通登录用户。\n");
        return;
    }
    printf("\n========== 选择普通登录用户 ==========\n");
    for (int index = 0; index < count; ++index)
    {
        printf("%d. %s（UID %lu，密码%s）\n",
               index + 1, users[index].username,
               (unsigned long)users[index].uid,
               password_state_text(account_password_state(users[index].username)));
    }
    printf("0. 取消\n");
    terminal_ui_set_step("选择要修改密码的用户",
                         "这里只列出可登录普通用户；root 使用单独的修改入口");
    selected = read_int("请选择用户: ", 0, count);
    if (selected == TERMINAL_UI_INPUT_CANCELLED || selected == 0)
    {
        printf("已取消。\n");
        return;
    }
    username = users[selected - 1].username;
    terminal_ui_set_step("设置普通用户密码",
                         "密码仅显示星号，不会进入命令参数或日志");
    if (!read_confirmed_password(password, sizeof(password)))
    {
        printf("已取消，用户密码没有变化。\n");
        return;
    }
    printf("准备修改用户“%s”的密码。\n", username);
    terminal_ui_set_step("确认修改用户密码",
                         "保存后该用户的旧密码立即失效");
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定修改吗？[y/N]: ", false))
    {
        secure_clear(password, sizeof(password));
        printf("已取消。\n");
        return;
    }
    if (set_account_password(username, password))
    {
        printf("[完成] 用户“%s”的密码已更新。\n", username);
    }
    else
    {
        printf("[失败] 系统拒绝了新密码，请检查 chpasswd、PAM 或系统密码质量策略。\n");
    }
    secure_clear(password, sizeof(password));
}

static void list_login_users(void)
{
    struct passwd *entry;
    uid_t uid_min;
    uid_t uid_max;
    int visible_count = 0;
    int hidden_count = 0;

    login_uid_range(&uid_min, &uid_max);
    printf("\n======================= 可登录用户 =======================\n");
    printf("%-20s %-8s %-10s %-8s %-24s %s\n",
           "用户名", "UID", "类型", "密码", "home", "登录shell");
    printf("----------------------------------------------------------\n");
    setpwent();
    while ((entry = getpwent()) != NULL)
    {
        char username[DISPLAY_ACCOUNT_NAME_SIZE];
        char home[128];
        char shell[128];
        uid_t uid = entry->pw_uid;
        bool login = account_can_login(entry->pw_shell);
        bool visible = uid == 0 ||
                       (login && uid >= uid_min && uid <= uid_max);
        const char *type;
        PasswordState password;

        if (!visible)
        {
            ++hidden_count;
            continue;
        }
        snprintf(username, sizeof(username), "%s", entry->pw_name);
        snprintf(home, sizeof(home), "%s", entry->pw_dir ? entry->pw_dir : "-");
        snprintf(shell, sizeof(shell), "%s", entry->pw_shell ? entry->pw_shell : "-");
        password = account_password_state(username);
        type = uid == 0 ? "root" : "普通用户";
        printf("%-20s %-8lu %-10s %-8s %-24s %s%s\n",
               username, (unsigned long)uid, type,
               password_state_text(password), home, shell,
               login ? "" : " [不可登录]");
        ++visible_count;
    }
    endpwent();
    printf("----------------------------------------------------------\n");
    printf("共 %d 个可登录用户。已隐藏 %d 个不可登录的系统服务账号。\n",
           visible_count, hidden_count);
    printf("密码列只显示状态，不会读取或显示密码内容。\n");
}

void manage_system_access(void)
{
    int choice;

    printf("\n====================================================\n");
    printf("                SSH 与系统用户管理\n");
    printf("====================================================\n");
    printf("1. 查看 SSH、root 和密码状态\n");
    printf("2. 允许 root 通过 SSH 登录（高风险）\n");
    printf("3. 设置或修改 root 密码\n");
    printf("4. 新建普通登录用户并设置密码\n");
    printf("5. 修改普通登录用户密码\n");
    printf("6. 查看所有可登录用户名\n");
    printf("0. 返回主菜单\n");
    printf("====================================================\n");
    terminal_ui_set_step("选择账号与 SSH 操作",
                         "密码不会明文显示；root SSH 需要完整确认词");
    choice = read_int("请选择 [0-6]: ", 0, 6);
    if (choice == TERMINAL_UI_INPUT_CANCELLED || choice == 0)
    {
        return;
    }
    switch (choice)
    {
    case 1:
        show_ssh_status();
        break;
    case 2:
        enable_root_ssh();
        break;
    case 3:
        set_root_password();
        break;
    case 4:
        create_login_user();
        break;
    case 5:
        modify_login_user_password();
        break;
    case 6:
        list_login_users();
        break;
    default:
        break;
    }
}
