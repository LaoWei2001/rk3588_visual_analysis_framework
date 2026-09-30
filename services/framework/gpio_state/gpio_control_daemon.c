#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef CONTROL_SOCKET
#define CONTROL_SOCKET "/run/rk3588-gpio-control/control.sock"
#endif
#ifndef RUNTIME_DIR
#define RUNTIME_DIR "/run/rk3588-gpio-control"
#endif
#ifndef STATE_DIR
#define STATE_DIR "/var/lib/rk3588-gpio"
#endif
#ifndef PERSISTENCE_ENABLE_FILE
#define PERSISTENCE_ENABLE_FILE "/run/rk3588-gpio-persistence/enabled"
#endif
#define LEGACY_GPIO_STATE "/var/lib/gpio6_a0_test.state"
#define LEGACY_RELAY_STATE "/var/lib/relay_test.state"
#define MAX_PINS 64
#define REQUEST_SIZE 128
#define RESPONSE_SIZE 256

struct held_pin {
    char name[32];
    unsigned int group;
    unsigned int offset;
    int direction;
    int value;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
};

static struct held_pin held_pins[MAX_PINS];
static size_t held_count;
static volatile sig_atomic_t stopping;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int parse_pin_name(const char *name, unsigned int *group,
                          unsigned int *offset)
{
    unsigned int parsed_group;
    unsigned int bit;
    char bank;
    int consumed = 0;

    if (!name || sscanf(name, "GPIO%u_%c%u%n", &parsed_group, &bank, &bit,
                        &consumed) != 3 || name[consumed] != '\0' ||
        bank < 'A' || bank > 'Z' || bit > 7)
    {
        errno = EINVAL;
        return -1;
    }
    *group = parsed_group;
    *offset = 8U * (unsigned int)(bank - 'A') + bit;
    return 0;
}

static struct held_pin *find_held_pin(const char *name)
{
    size_t index;
    for (index = 0; index < held_count; index++)
    {
        if (strcmp(held_pins[index].name, name) == 0)
            return &held_pins[index];
    }
    return NULL;
}

static int persistence_enabled(void)
{
    return access(PERSISTENCE_ENABLE_FILE, F_OK) == 0;
}

static int ensure_state_dir(void)
{
    struct stat info;

    if (mkdir(STATE_DIR, 0755) != 0 && errno != EEXIST)
        return -1;
    if (lstat(STATE_DIR, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0022) != 0)
    {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int state_path(const char *pin, char *path, size_t size)
{
    int count = snprintf(path, size, "%s/%s.state", STATE_DIR, pin);
    if (count <= 0 || (size_t)count >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/*
 * 持久状态只能由本后台在 GPIO 请求的串行处理区内更新。这样硬件命令的
 * 先后顺序与 .state 的提交顺序完全相同，不再由各客户端在收到响应后竞争写入。
 */
static int save_persistent_value(const char *pin, int value)
{
    char final_path[PATH_MAX];
    char temporary_path[PATH_MAX];
    char text[2] = {(char)('0' + (value ? 1 : 0)), '\n'};
    int fd = -1;
    int directory_fd = -1;
    int ok = 0;
    int result = -1;

    if (ensure_state_dir() != 0 ||
        state_path(pin, final_path, sizeof(final_path)) != 0 ||
        snprintf(temporary_path, sizeof(temporary_path), "%s/.%s.%ld.tmp",
                 STATE_DIR, pin, (long)getpid()) >= (int)sizeof(temporary_path))
        return -1;

    unlink(temporary_path);
    fd = open(temporary_path,
              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    ok = write(fd, text, sizeof(text)) == (ssize_t)sizeof(text) &&
         fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    fd = -1;
    if (ok && rename(temporary_path, final_path) == 0)
    {
        directory_fd = open(STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd >= 0 && fsync(directory_fd) == 0)
            result = 0;
    }
    if (directory_fd >= 0)
        close(directory_fd);
    if (result != 0)
        unlink(temporary_path);
    return result;
}

static int unlink_if_exists(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

static int clear_persistent_output(const char *pin)
{
    char path[PATH_MAX];

    if (state_path(pin, path, sizeof(path)) != 0 ||
        unlink_if_exists(path) != 0)
        return -1;
    if (strcmp(pin, "GPIO6_A0") == 0 &&
        unlink_if_exists(LEGACY_GPIO_STATE) != 0)
        return -1;
    if (strcmp(pin, "GPIO6_A2") == 0 &&
        unlink_if_exists(LEGACY_RELAY_STATE) != 0)
        return -1;
    return 0;
}

static int save_runtime_level(const char *pin, int value)
{
    char final_path[PATH_MAX];
    char temporary_path[PATH_MAX];
    char text[2] = {(char)('0' + (value ? 1 : 0)), '\n'};
    int fd;

    if (snprintf(final_path, sizeof(final_path), "%s/%s.level", RUNTIME_DIR,
                 pin) >= (int)sizeof(final_path) ||
        snprintf(temporary_path, sizeof(temporary_path), "%s/.%s.%ld.tmp",
                 RUNTIME_DIR, pin, (long)getpid()) >= (int)sizeof(temporary_path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    unlink(temporary_path);
    fd = open(temporary_path,
              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    ssize_t written = write(fd, text, sizeof(text));
    int write_errno = errno;
    int close_result = close(fd);
    if (written != (ssize_t)sizeof(text) || close_result != 0)
    {
        int saved_errno = written != (ssize_t)sizeof(text) ? write_errno : errno;
        unlink(temporary_path);
        errno = saved_errno;
        return -1;
    }
    if (rename(temporary_path, final_path) != 0)
    {
        int saved_errno = errno;
        unlink(temporary_path);
        errno = saved_errno;
        return -1;
    }
    {
        char input_path[PATH_MAX];
        if (snprintf(input_path, sizeof(input_path), "%s/%s.input",
                     RUNTIME_DIR, pin) < (int)sizeof(input_path))
            unlink(input_path);
    }
    return 0;
}

static int save_runtime_input(const char *pin)
{
    char input_path[PATH_MAX];
    char level_path[PATH_MAX];
    int fd;
    ssize_t written;
    int write_errno;
    int close_result;

    if (snprintf(input_path, sizeof(input_path), "%s/%s.input", RUNTIME_DIR,
                 pin) >= (int)sizeof(input_path) ||
        snprintf(level_path, sizeof(level_path), "%s/%s.level", RUNTIME_DIR,
                 pin) >= (int)sizeof(level_path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(input_path,
              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    written = write(fd, "input\n", 6);
    write_errno = errno;
    close_result = close(fd);
    if (written != 6 || close_result != 0)
    {
        if (written != 6)
            errno = write_errno;
        return -1;
    }
    unlink(level_path);
    return 0;
}

static int set_pin(const char *name, int value)
{
    struct held_pin *pin;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    unsigned int group;
    unsigned int offset;

    value = value ? 1 : 0;
    if (parse_pin_name(name, &group, &offset) != 0)
        return -1;

    pin = find_held_pin(name);
    if (pin)
    {
        if (pin->direction == GPIOD_LINE_DIRECTION_INPUT)
        {
            if (gpiod_line_set_direction_output(pin->line, value) != 0)
                return -1;
            pin->direction = GPIOD_LINE_DIRECTION_OUTPUT;
        }
        else if (gpiod_line_set_value(pin->line, value) != 0)
        {
            return -1;
        }
        pin->value = value;
        (void)save_runtime_level(name, value);
        return 0;
    }
    if (held_count >= MAX_PINS)
    {
        errno = ENOSPC;
        return -1;
    }
    chip = gpiod_chip_open_by_number(group);
    if (!chip)
        return -1;
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_request_output(line, "rk3588_gpio_control", value) != 0)
    {
        gpiod_chip_close(chip);
        return -1;
    }

    pin = &held_pins[held_count++];
    memset(pin, 0, sizeof(*pin));
    snprintf(pin->name, sizeof(pin->name), "%s", name);
    pin->group = group;
    pin->offset = offset;
    pin->direction = GPIOD_LINE_DIRECTION_OUTPUT;
    pin->value = value;
    pin->chip = chip;
    pin->line = line;
    (void)save_runtime_level(name, value);
    printf("[GPIO-Control] 持有 %s=%d\n", name, value);
    fflush(stdout);
    return 0;
}

static int set_pin_input(const char *name)
{
    struct held_pin *pin;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    unsigned int group;
    unsigned int offset;

    if (parse_pin_name(name, &group, &offset) != 0)
        return -1;
    pin = find_held_pin(name);
    if (pin)
    {
        if (pin->direction == GPIOD_LINE_DIRECTION_OUTPUT &&
            gpiod_line_set_direction_input(pin->line) != 0)
            return -1;
        pin->direction = GPIOD_LINE_DIRECTION_INPUT;
        (void)save_runtime_input(name);
        return 0;
    }
    if (held_count >= MAX_PINS)
    {
        errno = ENOSPC;
        return -1;
    }
    chip = gpiod_chip_open_by_number(group);
    if (!chip)
        return -1;
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_request_input(line, "rk3588_gpio_control") != 0)
    {
        gpiod_chip_close(chip);
        return -1;
    }
    pin = &held_pins[held_count++];
    memset(pin, 0, sizeof(*pin));
    snprintf(pin->name, sizeof(pin->name), "%s", name);
    pin->group = group;
    pin->offset = offset;
    pin->direction = GPIOD_LINE_DIRECTION_INPUT;
    pin->chip = chip;
    pin->line = line;
    (void)save_runtime_input(name);
    printf("[GPIO-Control] 持有 %s=input\n", name);
    fflush(stdout);
    return 0;
}

static int get_pin(const char *name, int *value)
{
    struct held_pin *pin = find_held_pin(name);
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    unsigned int group;
    unsigned int offset;
    int result;

    if (!value || parse_pin_name(name, &group, &offset) != 0)
        return -1;
    if (pin)
    {
        result = gpiod_line_get_value(pin->line);
        if (result < 0)
            return -1;
        *value = result;
        return 0;
    }

    chip = gpiod_chip_open_by_number(group);
    if (!chip)
        return -1;
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_request_input(line, "rk3588_gpio_control") != 0)
    {
        gpiod_chip_close(chip);
        return -1;
    }
    result = gpiod_line_get_value(line);
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    if (result < 0)
        return -1;
    *value = result;
    return 0;
}

/* READ 只允许读取真正处于输入方向、且没有被本服务保持为输出的线路。 */
static int read_input_pin(const char *name, int *value)
{
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    unsigned int group;
    unsigned int offset;
    int result;

    if (!value || parse_pin_name(name, &group, &offset) != 0)
        return -1;
    {
        struct held_pin *pin = find_held_pin(name);
        if (pin)
        {
            if (pin->direction != GPIOD_LINE_DIRECTION_INPUT)
            {
                errno = EBUSY;
                return -1;
            }
            result = gpiod_line_get_value(pin->line);
            if (result < 0)
                return -1;
            *value = result;
            return 0;
        }
    }
    chip = gpiod_chip_open_by_number(group);
    if (!chip)
        return -1;
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_direction(line) != GPIOD_LINE_DIRECTION_INPUT)
    {
        gpiod_chip_close(chip);
        errno = EBUSY;
        return -1;
    }
    if (gpiod_line_request_input(line, "rk3588_gpio_control") != 0)
    {
        gpiod_chip_close(chip);
        return -1;
    }
    result = gpiod_line_get_value(line);
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    if (result < 0)
        return -1;
    *value = result;
    return 0;
}

/* STATUS 只查询本服务正在持有的输出，不会临时申请输入或改变方向。 */
static int get_held_output(const char *name, int *value)
{
    struct held_pin *pin;
    unsigned int group;
    unsigned int offset;
    int result;

    if (!value || parse_pin_name(name, &group, &offset) != 0)
        return -1;
    (void)group;
    (void)offset;
    pin = find_held_pin(name);
    if (!pin || pin->direction != GPIOD_LINE_DIRECTION_OUTPUT)
    {
        errno = ENOENT;
        return -1;
    }
    result = gpiod_line_get_value(pin->line);
    if (result < 0)
        return -1;
    *value = result;
    return 0;
}

static void restore_runtime_modes(void)
{
    DIR *directory = opendir(RUNTIME_DIR);
    struct dirent *entry;
    if (!directory)
        return;
    while ((entry = readdir(directory)) != NULL)
    {
        char pin[32];
        char path[PATH_MAX];
        char value;
        int fd;
        size_t length = strlen(entry->d_name);
        const char *suffix;
        size_t suffix_length;
        size_t pin_length;

        if (length > strlen(".level") &&
            strcmp(entry->d_name + length - strlen(".level"), ".level") == 0)
            suffix = ".level";
        else if (length > strlen(".input") &&
                 strcmp(entry->d_name + length - strlen(".input"),
                        ".input") == 0)
            suffix = ".input";
        else
            continue;
        suffix_length = strlen(suffix);
        pin_length = length - suffix_length;
        if (pin_length >= sizeof(pin))
            continue;
        memcpy(pin, entry->d_name, pin_length);
        pin[pin_length] = '\0';
        {
            unsigned int group;
            unsigned int offset;
            if (parse_pin_name(pin, &group, &offset) != 0)
                continue;
        }
        if (strcmp(suffix, ".input") == 0)
        {
            (void)set_pin_input(pin);
        }
        else
        {
            if (snprintf(path, sizeof(path), "%s/%s", RUNTIME_DIR,
                         entry->d_name) >= (int)sizeof(path))
                continue;
            fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0)
                continue;
            if (read(fd, &value, 1) == 1 && (value == '0' || value == '1'))
                (void)set_pin(pin, value - '0');
            close(fd);
        }
    }
    closedir(directory);
}

static void reply_error(int client, int error_number)
{
    char response[RESPONSE_SIZE];
    if (error_number <= 0)
        error_number = EIO;
    snprintf(response, sizeof(response), "ERR %d %s", error_number,
             strerror(error_number));
    (void)send(client, response, strlen(response), MSG_NOSIGNAL);
}

/* GPIO 已经改变、但状态落盘失败时不能谎报为“完全没执行”。 */
static void reply_applied(int client, int value, int error_number)
{
    char response[RESPONSE_SIZE];
    if (error_number <= 0)
        error_number = EIO;
    snprintf(response, sizeof(response), "APPLIED %d %d %s", value ? 1 : 0,
             error_number, strerror(error_number));
    (void)send(client, response, strlen(response), MSG_NOSIGNAL);
}

static void handle_client(int client)
{
    char request[REQUEST_SIZE] = {0};
    char operation[16] = {0};
    char pin[32] = {0};
    char response[64];
    int value;
    ssize_t count = recv(client, request, sizeof(request) - 1, 0);
    if (count <= 0)
        return;
    request[count] = '\0';

    if (sscanf(request, "%15s %31s %d", operation, pin, &value) == 3 &&
        (strcmp(operation, "SET") == 0 ||
         strcmp(operation, "SET_MANAGED") == 0))
    {
        int managed = strcmp(operation, "SET_MANAGED") == 0;
        if (value != 0 && value != 1)
        {
            reply_error(client, EINVAL);
            return;
        }
        if (set_pin(pin, value) != 0)
        {
            reply_error(client, errno);
            return;
        }
        /* 返回真实逻辑回读，而不是简单回显客户端传入的 value。 */
        if (get_held_output(pin, &value) != 0)
        {
            reply_applied(client, value, errno);
            return;
        }
        if (managed && persistence_enabled() &&
            save_persistent_value(pin, value) != 0)
        {
            int persist_errno = errno;
            fprintf(stderr,
                    "[GPIO-Control] %s 已设置为 %d，但保存开机状态失败：%s\n",
                    pin, value, strerror(persist_errno));
            reply_applied(client, value, persist_errno);
            return;
        }
        snprintf(response, sizeof(response), "OK %d", value);
    }
    else if (sscanf(request, "%15s %31s", operation, pin) == 2 &&
             strcmp(operation, "GET") == 0)
    {
        if (get_pin(pin, &value) != 0)
        {
            reply_error(client, errno);
            return;
        }
        snprintf(response, sizeof(response), "OK %d", value);
    }
    else if (sscanf(request, "%15s %31s", operation, pin) == 2 &&
             strcmp(operation, "READ") == 0)
    {
        if (read_input_pin(pin, &value) != 0)
        {
            reply_error(client, errno);
            return;
        }
        snprintf(response, sizeof(response), "OK %d", value);
    }
    else if (sscanf(request, "%15s %31s", operation, pin) == 2 &&
             strcmp(operation, "STATUS") == 0)
    {
        if (get_held_output(pin, &value) != 0)
        {
            reply_error(client, errno);
            return;
        }
        snprintf(response, sizeof(response), "OK %d", value);
    }
    else if (sscanf(request, "%15s %31s", operation, pin) == 2 &&
             (strcmp(operation, "INPUT") == 0 ||
              strcmp(operation, "INPUT_MANAGED") == 0))
    {
        int managed = strcmp(operation, "INPUT_MANAGED") == 0;
        if (set_pin_input(pin) != 0)
        {
            reply_error(client, errno);
            return;
        }
        if (managed && persistence_enabled() &&
            clear_persistent_output(pin) != 0)
        {
            int persist_errno = errno;
            fprintf(stderr,
                    "[GPIO-Control] %s 已切换为输入，但清除开机输出状态失败：%s\n",
                    pin, strerror(persist_errno));
            reply_applied(client, 1, persist_errno);
            return;
        }
        snprintf(response, sizeof(response), "OK 1");
    }
    else if (strcmp(request, "PING") == 0)
    {
        snprintf(response, sizeof(response), "OK 1");
    }
    else
    {
        reply_error(client, EINVAL);
        return;
    }
    (void)send(client, response, strlen(response), MSG_NOSIGNAL);
}

static void cleanup(void)
{
    size_t index;
    for (index = 0; index < held_count; index++)
    {
        if (held_pins[index].line)
            gpiod_line_release(held_pins[index].line);
        if (held_pins[index].chip)
            gpiod_chip_close(held_pins[index].chip);
    }
    unlink(CONTROL_SOCKET);
}

int main(void)
{
    struct sockaddr_un address;
    struct sigaction action;
    int server;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (server < 0)
    {
        perror("socket");
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", CONTROL_SOCKET);
    unlink(CONTROL_SOCKET);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(CONTROL_SOCKET, 0660) != 0 || listen(server, 16) != 0)
    {
        perror("GPIO control socket");
        close(server);
        cleanup();
        return 1;
    }

    restore_runtime_modes();
    printf("[GPIO-Control] 实时控制服务已启动\n");
    fflush(stdout);
    while (!stopping)
    {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0)
        {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        handle_client(client);
        close(client);
    }
    close(server);
    cleanup();
    return 0;
}
