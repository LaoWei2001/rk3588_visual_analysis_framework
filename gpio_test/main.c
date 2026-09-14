#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PIN "GPIO6_A0"
#define GPIO_CONSUMER "rk3588_gpio_state"
#define STATE_DIR "/var/lib/rk3588-gpio"
#define PERSISTENCE_ENABLE_FILE "/run/rk3588-gpio-persistence/enabled"
#define CONTROL_SOCKET "/run/rk3588-gpio-control/control.sock"
#define LEGACY_GPIO_STATE "/var/lib/gpio6_a0_test.state"
#define LEGACY_RELAY_STATE "/var/lib/relay_test.state"

static int persistence_enabled(void)
{
    return access(PERSISTENCE_ENABLE_FILE, F_OK) == 0;
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
        return -1;
    }
    *group = parsed_group;
    *offset = 8U * (unsigned int)(bank - 'A') + bit;
    return 0;
}

struct gpio_chip_number
{
    unsigned int number;
};

static int compare_chip_number(const void *left, const void *right)
{
    const struct gpio_chip_number *a = left;
    const struct gpio_chip_number *b = right;
    return a->number > b->number ? 1 : (a->number < b->number ? -1 : 0);
}

static int is_framework_consumer(const char *consumer)
{
    return consumer &&
           (strcmp(consumer, "rk3588_gpio_control") == 0 ||
            strcmp(consumer, GPIO_CONSUMER) == 0);
}

static int list_gpio_lines(int argc, char **argv)
{
    struct gpio_chip_number chips[256];
    size_t chip_count = 0;
    DIR *directory;
    struct dirent *entry;
    int only_available = 0;
    int chip_filter = -1;
    int argument;
    unsigned int available_count = 0;
    unsigned int busy_count = 0;

    for (argument = 0; argument < argc; argument++)
    {
        if (strcmp(argv[argument], "--available") == 0)
        {
            only_available = 1;
        }
        else if (strcmp(argv[argument], "--chip") == 0 && argument + 1 < argc)
        {
            char *end = NULL;
            unsigned long number = strtoul(argv[++argument], &end, 10);
            if (!end || *end != '\0' || number > INT_MAX)
            {
                fprintf(stderr, "无效 gpiochip 编号：%s\n", argv[argument]);
                return 2;
            }
            chip_filter = (int)number;
        }
        else
        {
            fprintf(stderr, "list 仅支持 --available 和 --chip <编号>\n");
            return 2;
        }
    }

    directory = opendir("/dev");
    if (!directory)
    {
        fprintf(stderr, "无法扫描 /dev：%s\n", strerror(errno));
        return 1;
    }
    while ((entry = readdir(directory)) != NULL &&
           chip_count < sizeof(chips) / sizeof(chips[0]))
    {
        unsigned int number;
        int consumed = 0;
        if (sscanf(entry->d_name, "gpiochip%u%n", &number, &consumed) == 1 &&
            entry->d_name[consumed] == '\0')
            chips[chip_count++].number = number;
    }
    closedir(directory);
    qsort(chips, chip_count, sizeof(chips[0]), compare_chip_number);

    printf("状态说明：可控-空闲=候选线路；可控-本框架=可直接通过本工具控制；"
           "禁止-已占用=不要测试\n");
    printf("%-12s %-6s %-22s %-8s %-16s %-22s %s\n",
           "GPIO名称", "偏移", "线路名称", "方向", "状态", "占用者", "gpiochip/标签");

    for (size_t chip_index = 0; chip_index < chip_count; chip_index++)
    {
        unsigned int chip_number = chips[chip_index].number;
        struct gpiod_chip *chip;
        const char *chip_name;
        const char *chip_label;
        unsigned int line_count;

        if (chip_filter >= 0 && chip_number != (unsigned int)chip_filter)
            continue;
        chip = gpiod_chip_open_by_number(chip_number);
        if (!chip)
        {
            fprintf(stderr, "无法打开 gpiochip%u：%s\n", chip_number,
                    strerror(errno));
            continue;
        }
        chip_name = gpiod_chip_name(chip);
        chip_label = gpiod_chip_label(chip);
        line_count = gpiod_chip_num_lines(chip);

        for (unsigned int offset = 0; offset < line_count; offset++)
        {
            struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
            const char *line_name;
            const char *consumer;
            const char *direction;
            const char *status;
            char gpio_name[32];
            int used;
            int framework_owned;

            if (!line)
                continue;
            line_name = gpiod_line_name(line);
            consumer = gpiod_line_consumer(line);
            used = gpiod_line_is_used(line) ? 1 : 0;
            framework_owned = is_framework_consumer(consumer);
            if (only_available && used && !framework_owned)
            {
                busy_count++;
                continue;
            }
            if (offset / 8U < 26U)
                snprintf(gpio_name, sizeof(gpio_name), "GPIO%u_%c%u", chip_number,
                         (char)('A' + offset / 8U), offset % 8U);
            else
                snprintf(gpio_name, sizeof(gpio_name), "GPIO%u@%u", chip_number,
                         offset);
            direction = gpiod_line_direction(line) == GPIOD_LINE_DIRECTION_OUTPUT
                            ? "output" : "input";
            if (!used)
            {
                status = "可控-空闲";
                available_count++;
            }
            else if (framework_owned)
            {
                status = "可控-本框架";
                available_count++;
            }
            else
            {
                status = "禁止-已占用";
                busy_count++;
            }
            printf("%-12s %-6u %-22s %-8s %-16s %-22s %s/%s\n",
                   gpio_name, offset, line_name ? line_name : "-", direction,
                   status, consumer ? consumer : "-",
                   chip_name ? chip_name : "-", chip_label ? chip_label : "-");
        }
        gpiod_chip_close(chip);
    }
    if (chip_count == 0)
    {
        fprintf(stderr, "没有发现 /dev/gpiochip*；请检查内核 GPIO 驱动。\n");
        return 1;
    }
    printf("汇总：候选/本框架可控 %u，其他程序或内核占用 %u。\n",
           available_count, busy_count);
    printf("警告：空闲只表示当前未被内核占用，不代表板上已引出或电气上可安全翻转；"
           "实际测试前必须核对对应板卡原理图。\n");
    return 0;
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
    return count > 0 && (size_t)count < size ? 0 : -1;
}

static int read_value_file(const char *path, int *value)
{
    char buffer[8] = {0};
    struct stat info;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t count;

    if (fd < 0)
        return -1;
    count = read(fd, buffer, sizeof(buffer) - 1);
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
        count = -1;
    close(fd);
    if (count < 1 || (buffer[0] != '0' && buffer[0] != '1') ||
        (count > 1 && buffer[1] != '\n' && buffer[1] != '\0'))
    {
        errno = EINVAL;
        return -1;
    }
    *value = buffer[0] - '0';
    return 0;
}

static int load_value(const char *pin, int *value)
{
    char path[PATH_MAX];

    if (state_path(pin, path, sizeof(path)) != 0)
        return -1;
    return read_value_file(path, value);
}

static int save_value(const char *pin, int value)
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
    {
        return -1;
    }

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

/* 返回 0=后台已处理，1=后台未运行（调用方可直控），-1=后台返回错误。 */
static int controller_request(const char *operation, const char *pin,
                              int requested_value, int *returned_value)
{
    struct sockaddr_un address;
    char request[128];
    char response[256] = {0};
    int socket_fd;
    int count;
    int value;
    int remote_errno;

    socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return 1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", CONTROL_SOCKET);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        int connect_errno = errno;
        close(socket_fd);
        if (connect_errno == ENOENT || connect_errno == ECONNREFUSED)
            return 1;
        errno = connect_errno;
        return -1;
    }
    if (strcmp(operation, "SET") == 0)
        count = snprintf(request, sizeof(request), "SET %s %d", pin,
                         requested_value ? 1 : 0);
    else
        count = snprintf(request, sizeof(request), "GET %s", pin);
    if (count <= 0 || count >= (int)sizeof(request) ||
        send(socket_fd, request, (size_t)count, MSG_NOSIGNAL) != count)
    {
        int send_errno = errno;
        close(socket_fd);
        errno = send_errno;
        return -1;
    }
    count = (int)recv(socket_fd, response, sizeof(response) - 1, 0);
    close(socket_fd);
    if (count <= 0)
    {
        errno = EIO;
        return -1;
    }
    response[count] = '\0';
    if (sscanf(response, "OK %d", &value) == 1)
    {
        if (returned_value)
            *returned_value = value;
        return 0;
    }
    if (sscanf(response, "ERR %d", &remote_errno) == 1)
        errno = remote_errno;
    else
        errno = EPROTO;
    return -1;
}

static int read_line_as_input(const char *pin, int *value)
{
    char chip_name[32];
    struct gpiod_chip *chip = NULL;
    struct gpiod_line *line = NULL;
    unsigned int group;
    unsigned int offset;
    int result = -1;
    int controller_result = controller_request("GET", pin, 0, value);

    if (controller_result == 0)
        return 0;
    if (controller_result < 0)
        return -1;

    if (parse_pin_name(pin, &group, &offset) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    snprintf(chip_name, sizeof(chip_name), "gpiochip%u", group);
    chip = gpiod_chip_open_by_name(chip_name);
    if (!chip)
        goto cleanup;
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_request_input(line, GPIO_CONSUMER) < 0)
        goto cleanup;
    result = gpiod_line_get_value(line);
    if (result == 0 || result == 1)
        *value = result;
    else
        result = -1;

cleanup:
    if (line && gpiod_line_is_requested(line))
        gpiod_line_release(line);
    if (chip)
        gpiod_chip_close(chip);
    return result < 0 ? -1 : 0;
}

static int set_line(const char *pin, int value, int persist)
{
    char chip_name[32];
    struct gpiod_chip *chip = NULL;
    struct gpiod_line *line = NULL;
    unsigned int group;
    unsigned int offset;
    int readback;
    int result = -1;
    int controller_result;

    value = value ? 1 : 0;
    if (parse_pin_name(pin, &group, &offset) != 0)
    {
        fprintf(stderr, "无效 GPIO 名称：%s（格式应为 GPIOx_Yz）\n", pin);
        errno = EINVAL;
        return -1;
    }
    controller_result = controller_request("SET", pin, value, &readback);
    if (controller_result == 0)
    {
        if (readback != value)
        {
            errno = EIO;
            return -1;
        }
        if (persist && save_value(pin, value) != 0)
        {
            fprintf(stderr, "%s 已设置为 %d，但保存状态失败：%s\n",
                    pin, value, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (controller_result < 0)
    {
        fprintf(stderr, "实时 GPIO 控制服务无法设置 %s：%s\n",
                pin, strerror(errno));
        return -1;
    }
    snprintf(chip_name, sizeof(chip_name), "gpiochip%u", group);
    chip = gpiod_chip_open_by_name(chip_name);
    if (!chip)
    {
        fprintf(stderr, "无法打开 %s：%s\n", chip_name, strerror(errno));
        goto cleanup;
    }
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_request_output(line, GPIO_CONSUMER, value) < 0)
    {
        fprintf(stderr, "无法将 %s 设置为输出：%s\n", pin, strerror(errno));
        goto cleanup;
    }
    readback = gpiod_line_get_value(line);
    if (readback != value)
    {
        fprintf(stderr, "%s 输出校验失败，期望 %d，读取到 %d\n",
                pin, value, readback);
        goto cleanup;
    }
    if (persist && save_value(pin, value) != 0)
    {
        fprintf(stderr, "%s 已设置为 %d，但保存状态失败：%s\n",
                pin, value, strerror(errno));
        goto cleanup;
    }
    result = 0;

cleanup:
    if (line && gpiod_line_is_requested(line))
        gpiod_line_release(line);
    if (chip)
        gpiod_chip_close(chip);
    return result;
}

static int wait_for_gpio_chip(const char *pin, int timeout_ms)
{
    char device_path[64];
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    unsigned int group;
    unsigned int offset;
    int elapsed_ms = 0;

    if (parse_pin_name(pin, &group, &offset) != 0)
        return -1;
    (void)offset;
    snprintf(device_path, sizeof(device_path), "/dev/gpiochip%u", group);
    while (access(device_path, F_OK) != 0)
    {
        if (elapsed_ms >= timeout_ms)
            return -1;
        nanosleep(&delay, NULL);
        elapsed_ms += 20;
    }
    return 0;
}

static void migrate_legacy_state(const char *pin, const char *legacy_path)
{
    int value;

    if (load_value(pin, &value) == 0)
        return;
    if (read_value_file(legacy_path, &value) == 0)
        (void)save_value(pin, value);
}

static int restore_all(void)
{
    DIR *directory;
    struct dirent *entry;
    int restored = 0;
    int failures = 0;

    if (!persistence_enabled())
    {
        printf("GPIO 电平保持服务未开启，跳过状态恢复。\n");
        return 0;
    }
    if (ensure_state_dir() != 0)
    {
        fprintf(stderr, "无法准备状态目录 %s：%s\n", STATE_DIR,
                strerror(errno));
        return 1;
    }
    migrate_legacy_state("GPIO6_A0", LEGACY_GPIO_STATE);
    migrate_legacy_state("GPIO6_A2", LEGACY_RELAY_STATE);

    directory = opendir(STATE_DIR);
    if (!directory)
    {
        fprintf(stderr, "无法读取状态目录 %s：%s\n", STATE_DIR,
                strerror(errno));
        return 1;
    }
    while ((entry = readdir(directory)) != NULL)
    {
        char pin[32];
        int value;
        size_t length = strlen(entry->d_name);
        const char *suffix = ".state";
        size_t suffix_length = strlen(suffix);
        size_t pin_length;
        unsigned int group;
        unsigned int offset;

        if (length <= suffix_length ||
            strcmp(entry->d_name + length - suffix_length, suffix) != 0)
            continue;
        pin_length = length - suffix_length;
        if (pin_length >= sizeof(pin))
            continue;
        memcpy(pin, entry->d_name, pin_length);
        pin[pin_length] = '\0';
        if (parse_pin_name(pin, &group, &offset) != 0)
            continue;
        if (load_value(pin, &value) != 0)
        {
            fprintf(stderr, "忽略损坏的 GPIO 状态：%s\n", entry->d_name);
            failures++;
            continue;
        }
        if (wait_for_gpio_chip(pin, 2000) != 0 || set_line(pin, value, 0) != 0)
        {
            if (errno == ENOENT)
                fprintf(stderr, "等待 %s 对应的 GPIO 控制器超时\n", pin);
            failures++;
            continue;
        }
        printf("已恢复 %s=%d\n", pin, value);
        restored++;
    }
    closedir(directory);
    printf("GPIO 状态恢复完成：成功 %d，失败 %d\n", restored, failures);
    return failures == 0 ? 0 : 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "用法：\n"
            "  %s list [--available] [--chip <编号>]\n"
            "  %s [--pin GPIOx_Yz] set <0|1|low|high>\n"
            "  %s [--pin GPIOx_Yz] get\n"
            "  %s [--pin GPIOx_Yz] toggle\n"
            "  %s restore\n"
            "未指定 --pin 时使用 %s；不带参数时兼容旧行为并执行 toggle。\n",
            program, program, program, program, program, DEFAULT_PIN);
}

static int parse_value(const char *text, int *value)
{
    if (strcmp(text, "0") == 0 || strcmp(text, "low") == 0 ||
        strcmp(text, "off") == 0)
    {
        *value = 0;
        return 0;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "high") == 0 ||
        strcmp(text, "on") == 0)
    {
        *value = 1;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *pin = DEFAULT_PIN;
    const char *command = "toggle";
    int argument = 1;
    int value;
    int persist;

    if (argc > 1 && strcmp(argv[1], "list") == 0)
        return list_gpio_lines(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "restore") == 0)
    {
        if (argc != 2)
        {
            usage(argv[0]);
            return 2;
        }
        return restore_all();
    }
    if (argc > 2 && strcmp(argv[1], "--pin") == 0)
    {
        pin = argv[2];
        argument = 3;
    }
    if (argument < argc)
        command = argv[argument++];

    persist = persistence_enabled();
    if (persist)
    {
        migrate_legacy_state("GPIO6_A0", LEGACY_GPIO_STATE);
        migrate_legacy_state("GPIO6_A2", LEGACY_RELAY_STATE);
    }

    if (strcmp(command, "set") == 0)
    {
        if (argument >= argc || parse_value(argv[argument], &value) != 0)
        {
            usage(argv[0]);
            return 2;
        }
        argument++;
    }
    else if (parse_value(command, &value) == 0)
    {
        /* 允许简写：gpio_test 0 / gpio_test low。 */
    }
    else if (strcmp(command, "get") == 0)
    {
        if (persist && load_value(pin, &value) == 0)
        {
            if (set_line(pin, value, 0) != 0)
                return 1;
            printf("%s=%d（已保存并重新施加）\n", pin, value);
            return 0;
        }
        if (read_line_as_input(pin, &value) != 0)
        {
            fprintf(stderr, "无法读取 %s：%s\n", pin, strerror(errno));
            return 1;
        }
        printf("%s=%d（%s）\n", pin, value,
               persist ? "尚未保存" : "保持服务已关闭，读取当前电平");
        return 0;
    }
    else if (strcmp(command, "toggle") == 0)
    {
        if ((!persist || load_value(pin, &value) != 0) &&
            read_line_as_input(pin, &value) != 0)
        {
            fprintf(stderr, "无法确定 %s 当前状态：%s\n", pin,
                    strerror(errno));
            return 1;
        }
        value = 1 - value;
    }
    else
    {
        usage(argv[0]);
        return 2;
    }

    if (argument != argc || set_line(pin, value, persist) != 0)
    {
        if (argument != argc)
            usage(argv[0]);
        return argument != argc ? 2 : 1;
    }
    if (persist)
        printf("%s 已设置并保存为 %d\n", pin, value);
    else
        printf("%s 已设置为 %d（保持服务已关闭，不保存）\n", pin, value);
    return 0;
}
