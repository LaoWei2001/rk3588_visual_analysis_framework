/*
 * Copyright (C) 2026 JNU IOT C301 Sunny_Wei, all rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PIN "GPIO6_A0"
#define GPIO_CONSUMER "rk3588_gpio_state"
#ifndef STATE_DIR
#define STATE_DIR "/var/lib/rk3588-gpio"
#endif
#ifndef RUNTIME_LEVEL_DIR
#define RUNTIME_LEVEL_DIR "/run/rk3588-gpio-control"
#endif
#ifndef PERSISTENCE_ENABLE_FILE
#define PERSISTENCE_ENABLE_FILE "/run/rk3588-gpio-persistence/enabled"
#endif
#ifndef CONTROL_SOCKET
#define CONTROL_SOCKET "/run/rk3588-gpio-control/control.sock"
#endif
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

struct gpio_power_info
{
    const char *domain;
    const char *high_voltage;
};

struct gpio_controller_info
{
    int is_rk3588_bank;
    char identity[96];
    char power_domain[96];
    char high_voltage[96];
};

static int rk3588_gpio_bank(const char *chip_label, unsigned int *bank);

static int join_path(char *destination, size_t destination_size,
                     const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);

    if (directory_length + 1 + name_length + 1 > destination_size)
        return -1;
    memcpy(destination, directory, directory_length);
    destination[directory_length] = '/';
    memcpy(destination + directory_length + 1, name, name_length + 1);
    return 0;
}

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    size_t count;

    if (destination_size == 0)
        return;
    count = strlen(source);
    if (count >= destination_size)
        count = destination_size - 1;
    memcpy(destination, source, count);
    destination[count] = '\0';
}

/* printf 的字段宽度按字节计算，会把 UTF-8 中文表格排歪；这里按终端宽度补空格。 */
static size_t terminal_text_width(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    size_t width = 0;

    while (*cursor)
    {
        if (*cursor < 0x80)
        {
            width++;
            cursor++;
        }
        else
        {
            size_t bytes = (*cursor & 0xf8) == 0xf0 ? 4 :
                           (*cursor & 0xf0) == 0xe0 ? 3 :
                           (*cursor & 0xe0) == 0xc0 ? 2 : 1;
            size_t index;

            /* 本工具的非 ASCII 文本是中日韩字符或全角标点，终端宽度均为 2。 */
            width += 2;
            cursor++;
            for (index = 1; index < bytes && (*cursor & 0xc0) == 0x80;
                 index++)
                cursor++;
        }
    }
    return width;
}

static void print_table_cell(const char *text, size_t column_width)
{
    size_t width = terminal_text_width(text);

    fputs(text, stdout);
    while (width++ < column_width)
        putchar(' ');
    putchar(' ');
}

static int device_tree_has_compatible(const char *compatible)
{
    char buffer[1024];
    ssize_t count;
    size_t offset = 0;
    int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    count = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (count <= 0)
        return 0;

    while (offset < (size_t)count)
    {
        size_t remaining = (size_t)count - offset;
        size_t length = strnlen(buffer + offset, remaining);

        if (length == strlen(compatible) &&
            memcmp(buffer + offset, compatible, length) == 0)
            return 1;
        if (length == remaining)
            break;
        offset += length + 1;
    }
    return 0;
}

static int read_first_property_string(const char *path, char *value,
                                      size_t value_size)
{
    ssize_t count;
    int fd;

    if (value_size == 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    count = read(fd, value, value_size - 1);
    close(fd);
    if (count <= 0)
        return -1;
    value[count] = '\0';
    return 0;
}

static int read_device_tree_u32(const char *path, uint32_t *value)
{
    unsigned char bytes[4];
    ssize_t count;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    count = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (count != (ssize_t)sizeof(bytes))
        return -1;
    *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
             ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
    return 0;
}

static int string_has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}

static int find_device_tree_node_by_phandle(const char *directory,
                                            uint32_t wanted,
                                            char *node_path,
                                            size_t node_path_size,
                                            unsigned int depth)
{
    const char *properties[] = {"phandle", "linux,phandle"};
    char path[PATH_MAX];
    DIR *stream;
    struct dirent *entry;

    if (depth > 32)
        return -1;
    for (size_t index = 0; index < sizeof(properties) / sizeof(properties[0]);
         index++)
    {
        uint32_t value;
        if (snprintf(path, sizeof(path), "%s/%s", directory,
                     properties[index]) < (int)sizeof(path) &&
            read_device_tree_u32(path, &value) == 0 && value == wanted)
        {
            if (snprintf(node_path, node_path_size, "%s", directory) >=
                (int)node_path_size)
                return -1;
            return 0;
        }
    }

    stream = opendir(directory);
    if (!stream)
        return -1;
    while ((entry = readdir(stream)) != NULL)
    {
        struct stat info;

        if (entry->d_name[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", directory,
                     entry->d_name) >= (int)sizeof(path) ||
            lstat(path, &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        if (find_device_tree_node_by_phandle(path, wanted, node_path,
                                             node_path_size, depth + 1) == 0)
        {
            closedir(stream);
            return 0;
        }
    }
    closedir(stream);
    return -1;
}

static int find_supply_property(const char *of_node_path,
                                char *property_path,
                                size_t property_path_size,
                                char *supply_name,
                                size_t supply_name_size)
{
    char first_property[NAME_MAX + 1] = {0};
    DIR *stream = opendir(of_node_path);
    struct dirent *entry;

    if (!stream)
        return -1;
    while ((entry = readdir(stream)) != NULL)
    {
        if (!string_has_suffix(entry->d_name, "-supply"))
            continue;
        if (strcmp(entry->d_name, "vcc-supply") == 0)
        {
            snprintf(first_property, sizeof(first_property), "%s",
                     entry->d_name);
            break;
        }
        if (first_property[0] == '\0')
            snprintf(first_property, sizeof(first_property), "%s",
                     entry->d_name);
    }
    closedir(stream);
    if (first_property[0] == '\0' ||
        snprintf(property_path, property_path_size, "%s/%s", of_node_path,
                 first_property) >= (int)property_path_size)
        return -1;

    snprintf(supply_name, supply_name_size, "%.*s",
             (int)(strlen(first_property) - strlen("-supply")),
             first_property);
    return 0;
}

static int find_gpiochip_driver(const char *sysfs_link, char *driver,
                                size_t driver_size)
{
    char current[PATH_MAX];

    if (!realpath(sysfs_link, current))
        return -1;
    while (strncmp(current, "/sys/devices/", strlen("/sys/devices/")) == 0)
    {
        char driver_link[PATH_MAX];
        char resolved[PATH_MAX];
        char *name;
        char *slash;

        if (snprintf(driver_link, sizeof(driver_link), "%s/driver", current) <
                (int)sizeof(driver_link) &&
            realpath(driver_link, resolved))
        {
            name = strrchr(resolved, '/');
            name = name ? name + 1 : resolved;
            /* gpio_stub_drv 是 GPIO 核心的通用包装，继续向父设备找真实驱动。 */
            if (strcmp(name, "gpio_stub_drv") != 0)
            {
                copy_text(driver, driver_size, name);
                return 0;
            }
        }
        slash = strrchr(current, '/');
        if (!slash)
            break;
        *slash = '\0';
    }
    return -1;
}

static void friendly_compatible_name(const char *compatible, char *name,
                                     size_t name_size)
{
    const char *model = strrchr(compatible, ',');
    size_t index;

    model = model ? model + 1 : compatible;
    snprintf(name, name_size, "%s", model);
    if (strncmp(model, "pca", 3) == 0 || strncmp(model, "tca", 3) == 0 ||
        strncmp(model, "mcp", 3) == 0)
    {
        for (index = 0; name[index] != '\0'; index++)
            name[index] = (char)toupper((unsigned char)name[index]);
    }
}

static void describe_gpio_controller(unsigned int chip_number,
                                     const char *chip_label,
                                     int is_rk3588,
                                     struct gpio_controller_info *controller)
{
    char device_path[64];
    char sysfs_path[PATH_MAX];
    char compatible_path[PATH_MAX];
    char compatible[96] = {0};
    char driver[64] = {0};
    char model[64] = {0};
    char supply_property[PATH_MAX];
    char supply_name[64] = {0};
    char regulator_node[PATH_MAX];
    char regulator_name[96] = {0};
    uint32_t supply_phandle;
    uint32_t minimum_uv;
    uint32_t maximum_uv;
    struct stat device_info;
    unsigned int bank;

    memset(controller, 0, sizeof(*controller));
    snprintf(device_path, sizeof(device_path), "/dev/gpiochip%u", chip_number);
    if (stat(device_path, &device_info) == 0 && S_ISCHR(device_info.st_mode))
        snprintf(sysfs_path, sizeof(sysfs_path), "/sys/dev/char/%u:%u",
                 major(device_info.st_rdev), minor(device_info.st_rdev));
    else
        snprintf(sysfs_path, sizeof(sysfs_path), "/sys/dev/char/254:%u",
                 chip_number);

    if (join_path(compatible_path, sizeof(compatible_path), sysfs_path,
                  "of_node/compatible") == 0)
        (void)read_first_property_string(compatible_path, compatible,
                                         sizeof(compatible));
    (void)find_gpiochip_driver(sysfs_path, driver, sizeof(driver));

    if (is_rk3588 &&
        (strcmp(compatible, "rockchip,gpio-bank") == 0 ||
         strcmp(driver, "rockchip-gpio") == 0) &&
        rk3588_gpio_bank(chip_label, &bank) == 0)
    {
        controller->is_rk3588_bank = 1;
        snprintf(controller->identity, sizeof(controller->identity),
                 "rockchip,rk3588/gpio%u", bank);
        return;
    }

    if (compatible[0] != '\0')
    {
        snprintf(controller->identity, sizeof(controller->identity), "%s",
                 compatible);
        friendly_compatible_name(compatible, model, sizeof(model));
    }
    else if (driver[0] != '\0')
    {
        snprintf(controller->identity, sizeof(controller->identity),
                 "driver:%s", driver);
        snprintf(model, sizeof(model), "%s", driver);
    }
    else
    {
        snprintf(controller->identity, sizeof(controller->identity),
                 "未识别");
        snprintf(model, sizeof(model), "%s",
                 chip_label && chip_label[0] ? chip_label : "未知控制器");
    }

    if (strstr(model, "rk806") || strstr(model, "RK806"))
    {
        snprintf(controller->power_domain, sizeof(controller->power_domain),
                 "RK806 GPIO域");
        snprintf(controller->high_voltage, sizeof(controller->high_voltage),
                 "需查RK806电路*");
        return;
    }

    snprintf(controller->power_domain, sizeof(controller->power_domain),
             "%s供电", model);
    if (snprintf(compatible_path, sizeof(compatible_path), "%s/of_node",
                 sysfs_path) >= (int)sizeof(compatible_path) ||
        find_supply_property(compatible_path, supply_property,
                             sizeof(supply_property), supply_name,
                             sizeof(supply_name)) != 0)
    {
        snprintf(controller->high_voltage, sizeof(controller->high_voltage),
                 "供电未描述*");
        return;
    }

    if (read_device_tree_u32(supply_property, &supply_phandle) != 0 ||
        find_device_tree_node_by_phandle("/sys/firmware/devicetree/base",
                                        supply_phandle, regulator_node,
                                        sizeof(regulator_node), 0) != 0)
    {
        snprintf(controller->high_voltage, sizeof(controller->high_voltage),
                 "%s供电，电压未知*", supply_name);
        return;
    }

    if (join_path(compatible_path, sizeof(compatible_path), regulator_node,
                  "regulator-name") == 0)
    (void)read_first_property_string(compatible_path, regulator_name,
                                         sizeof(regulator_name));
    if (regulator_name[0] != '\0')
    {
        size_t used;

        copy_text(controller->power_domain, sizeof(controller->power_domain),
                  model);
        used = strlen(controller->power_domain);
        if (used + 1 < sizeof(controller->power_domain))
        {
            controller->power_domain[used++] = '/';
            controller->power_domain[used] = '\0';
            copy_text(controller->power_domain + used,
                      sizeof(controller->power_domain) - used,
                      regulator_name);
        }
    }

    if (join_path(compatible_path, sizeof(compatible_path), regulator_node,
                  "regulator-min-microvolt") != 0 ||
        read_device_tree_u32(compatible_path, &minimum_uv) != 0)
        minimum_uv = 0;
    if (join_path(compatible_path, sizeof(compatible_path), regulator_node,
                  "regulator-max-microvolt") != 0 ||
        read_device_tree_u32(compatible_path, &maximum_uv) != 0)
        maximum_uv = 0;

    if (minimum_uv != 0 && minimum_uv == maximum_uv)
        snprintf(controller->high_voltage, sizeof(controller->high_voltage),
                 "约%.2fV（设备树）", minimum_uv / 1000000.0);
    else if (minimum_uv != 0 && maximum_uv != 0)
        snprintf(controller->high_voltage, sizeof(controller->high_voltage),
                 "%.2f-%.2fV范围*", minimum_uv / 1000000.0,
                 maximum_uv / 1000000.0);
    else
        snprintf(controller->high_voltage, sizeof(controller->high_voltage),
                 "%s供电，电压未知*", supply_name);
}

static int rk3588_gpio_bank(const char *chip_label, unsigned int *bank)
{
    unsigned int parsed_bank;
    int consumed = 0;

    if (!chip_label ||
        sscanf(chip_label, "gpio%u%n", &parsed_bank, &consumed) != 1 ||
        chip_label[consumed] != '\0' || parsed_bank > 4)
        return -1;
    *bank = parsed_bank;
    return 0;
}

/*
 * RK3588 的 GPIO bank 跨多个 IO 电源域，不能只根据 bank 判断电压。
 * 下列边界来自 RK3588 数据手册中的管脚电源域和 Linux pinctrl 的 IOC
 * 寄存器分段。这里只给出芯片能力；带 * 的域实际接成 1.8 V 还是 3.3 V
 * 由载板硬件决定，GPIO 字符设备接口无法读取该板级连接。
 */
static struct gpio_power_info rk3588_gpio_power(unsigned int bank,
                                                unsigned int offset)
{
    switch (bank)
    {
    case 0:
        if (offset <= 12) /* GPIO0_A0..GPIO0_B4 */
            return (struct gpio_power_info){"PMUIO1", "约1.8V（固定）"};
        return (struct gpio_power_info){"PMUIO2", "1.8V或3.3V*"};
    case 1:
        if (offset <= 15 || offset >= 30) /* A0..B7、D6..D7 */
            return (struct gpio_power_info){"VCCIO4", "1.8V或3.3V*"};
        return (struct gpio_power_info){"VCCIO1", "约1.8V（固定）"};
    case 2:
        if (offset <= 5 || offset >= 24) /* A0..A5、D0..D7 */
            return (struct gpio_power_info){"EMMCIO", "约1.8V（固定）"};
        return (struct gpio_power_info){"VCCIO3", "约1.8V（固定）"};
    case 3:
        return (struct gpio_power_info){"VCCIO5", "1.8V或3.3V*"};
    case 4:
        if (offset <= 17) /* A0..C1 */
            return (struct gpio_power_info){"VCCIO6", "1.8V或3.3V*"};
        if (offset <= 23) /* C2..C7 */
            return (struct gpio_power_info){"VCCIO3", "约1.8V（固定）"};
        return (struct gpio_power_info){"VCCIO2", "1.8V或3.3V*"};
    default:
        return (struct gpio_power_info){"未知", "无法判断*"};
    }
}

static struct gpio_power_info gpio_power_info_for_chip(
    const struct gpio_controller_info *controller, const char *chip_label,
    unsigned int offset)
{
    unsigned int bank;

    if (controller->is_rk3588_bank &&
        rk3588_gpio_bank(chip_label, &bank) == 0)
        return rk3588_gpio_power(bank, offset);
    return (struct gpio_power_info){controller->power_domain,
                                    controller->high_voltage};
}

static void print_voltage_help(void)
{
    printf("\n电压结果怎么理解：\n");
    printf("  1. 约1.8V（固定）：RK3588 芯片规定该电源域只能使用 1.8V，"
           "输出高电平约为 1.8V。\n");
    printf("  2. 1.8V或3.3V*：表示载板设计时只会选择其中一种电压，"
           "不是同时输出两种，也不代表当前一定是 3.3V。\n");
    printf("  3. 约x.xxV（设备树）：程序沿 gpiochip 的 compatible 和 supply "
           "关系找到了固定稳压器；这是标称配置值，不是实测值。\n");
    printf("  4. 供电未描述*：控制器已经从设备树 compatible 或驱动中识别，"
           "但设备树没有说明它的供电来源；不是让用户外接 VCC。\n");
    printf("  5. 需查RK806电路*：GPIO 来自电源管理芯片 RK806，"
           "不能套用 RK3588 的 VCCIO 电压。\n");
    printf("  6. x.xx-y.yyV范围* 或电压未知*：设备树只给出了范围或供电引用，"
           "仍然不能当成当前实际电压。\n");
    printf("  * 星号表示：当前设备树没有提供对应的板级供电电压。接线前请查本机原理图；"
           "没有原理图时，用万用表测量该引脚对 GND 的高电平电压。\n");
    printf("  系统能读取 GPIO 的逻辑值 0/1，但逻辑值 1 只表示“高”，"
           "GPIO 接口不会返回它究竟是多少伏。\n");
    printf("  注意：表中是空载附近的逻辑高电平，不是供电输出端；GPIO 不能用来"
           "直接给大电流负载供电。\n");
}

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
    int is_rk3588 = device_tree_has_compatible("rockchip,rk3588");

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
    printf("电压说明：高电平约等于对应 IO 供电；详细解释见列表底部。\n");
    print_table_cell("GPIO名称", 12);
    print_table_cell("偏移", 6);
    print_table_cell("电源域", 18);
    print_table_cell("高电平约", 20);
    print_table_cell("线路名称", 32);
    print_table_cell("方向", 8);
    print_table_cell("状态", 14);
    print_table_cell("占用者", 32);
    printf("gpiochip/标签/动态识别\n");

    for (size_t chip_index = 0; chip_index < chip_count; chip_index++)
    {
        unsigned int chip_number = chips[chip_index].number;
        struct gpiod_chip *chip;
        const char *chip_name;
        const char *chip_label;
        struct gpio_controller_info controller;
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
        describe_gpio_controller(chip_number, chip_label, is_rk3588,
                                 &controller);

        for (unsigned int offset = 0; offset < line_count; offset++)
        {
            struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
            const char *line_name;
            const char *consumer;
            const char *direction;
            const char *status;
            char gpio_name[32];
            struct gpio_power_info power;
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
            power = gpio_power_info_for_chip(&controller, chip_label, offset);
            char offset_text[16];
            snprintf(offset_text, sizeof(offset_text), "%u", offset);
            print_table_cell(gpio_name, 12);
            print_table_cell(offset_text, 6);
            print_table_cell(power.domain, 18);
            print_table_cell(power.high_voltage, 20);
            print_table_cell(line_name ? line_name : "-", 32);
            print_table_cell(direction, 8);
            print_table_cell(status, 14);
            print_table_cell(consumer ? consumer : "-", 32);
            printf("%s/%s/%s\n", chip_name ? chip_name : "-",
                   chip_label ? chip_label : "-", controller.identity);
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
    print_voltage_help();
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

static int unlink_if_exists(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT)
        return 0;
    return -1;
}

static int clear_saved_output(const char *pin)
{
    char path[PATH_MAX];

    if (state_path(pin, path, sizeof(path)) != 0 ||
        unlink_if_exists(path) != 0)
        return -1;

    /* 旧版固定引脚状态也要一并清理，否则下次运行会再次迁移回来。 */
    if (strcmp(pin, "GPIO6_A0") == 0 &&
        unlink_if_exists(LEGACY_GPIO_STATE) != 0)
        return -1;
    if (strcmp(pin, "GPIO6_A2") == 0 &&
        unlink_if_exists(LEGACY_RELAY_STATE) != 0)
        return -1;
    return 0;
}

static int load_runtime_value(const char *pin, int *value)
{
    char path[PATH_MAX];
    int count = snprintf(path, sizeof(path), "%s/%s.level",
                         RUNTIME_LEVEL_DIR, pin);

    if (count <= 0 || count >= (int)sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return read_value_file(path, value);
}

static void clear_runtime_output_record(const char *pin)
{
    char path[PATH_MAX];
    int count = snprintf(path, sizeof(path), "%s/%s.level",
                         RUNTIME_LEVEL_DIR, pin);

    if (count > 0 && count < (int)sizeof(path))
        (void)unlink(path);
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

/*
 * 返回 0=后台完整处理成功，1=后台未运行（调用方可直控），
 * -1=后台未执行，-2=GPIO 已改变但统一状态落盘失败。
 */
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
    if (strcmp(operation, "SET") == 0 ||
        strcmp(operation, "SET_MANAGED") == 0)
        count = snprintf(request, sizeof(request), "%s %s %d", operation, pin,
                         requested_value ? 1 : 0);
    else if (strcmp(operation, "INPUT") == 0 ||
             strcmp(operation, "INPUT_MANAGED") == 0)
        count = snprintf(request, sizeof(request), "%s %s", operation, pin);
    else if (strcmp(operation, "GET") == 0 ||
             strcmp(operation, "READ") == 0 ||
             strcmp(operation, "STATUS") == 0)
        count = snprintf(request, sizeof(request), "%s %s", operation, pin);
    else
    {
        close(socket_fd);
        errno = EINVAL;
        return -1;
    }
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
    if (sscanf(response, "APPLIED %d %d", &value, &remote_errno) == 2)
    {
        if (returned_value)
            *returned_value = value;
        errno = remote_errno > 0 ? remote_errno : EIO;
        return -2;
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
    int controller_result = controller_request("READ", pin, 0, value);

    if (controller_result == 0)
        return 0;
    /* 旧后台不认识 READ，会返回 EINVAL；此时仍可安全走本地输入检查。 */
    if (controller_result < 0 && errno != EINVAL && errno != EPROTO)
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
    if (!line || gpiod_line_direction(line) != GPIOD_LINE_DIRECTION_INPUT)
    {
        errno = EBUSY;
        goto cleanup;
    }
    if (gpiod_line_request_input(line, GPIO_CONSUMER) < 0)
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

/*
 * 返回 0=由后台持续保持输入，1=本地完成切换但命令退出后不再占用，-1=失败。
 * managed_state=1 表示方向和持久状态已经由新版后台在同一请求内处理。
 */
static int set_line_input(const char *pin, int *managed_state)
{
    char chip_name[32];
    struct gpiod_chip *chip = NULL;
    struct gpiod_line *line = NULL;
    unsigned int group;
    unsigned int offset;
    int controller_result;
    int result = -1;

    if (parse_pin_name(pin, &group, &offset) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (managed_state)
        *managed_state = 0;
    controller_result = controller_request("INPUT_MANAGED", pin, 0, NULL);
    if (controller_result == 0)
    {
        if (managed_state)
            *managed_state = 1;
        return 0;
    }
    if (controller_result == -2)
    {
        fprintf(stderr,
                "%s 已切换为输入，但后台清除统一状态失败：%s\n",
                pin, strerror(errno));
        return -1;
    }
    if (controller_result < 0 && (errno == EINVAL || errno == EPROTO))
    {
        /* 兼容旧后台：退回旧 INPUT，持久状态仍由本工具维护。 */
        controller_result = controller_request("INPUT", pin, 0, NULL);
        if (controller_result == 0)
            return 0;
    }
    /* 后台未运行或连旧 INPUT 也不支持时，空闲线路可由本程序直接切换。 */
    if (controller_result < 0 && errno != EINVAL && errno != EPROTO)
        return -1;

    snprintf(chip_name, sizeof(chip_name), "gpiochip%u", group);
    chip = gpiod_chip_open_by_name(chip_name);
    if (!chip)
        goto cleanup;
    line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_request_input(line, GPIO_CONSUMER) < 0)
        goto cleanup;
    result = 1;

cleanup:
    if (line && gpiod_line_is_requested(line))
        gpiod_line_release(line);
    if (chip)
        gpiod_chip_close(chip);
    return result;
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
    controller_result = controller_request("SET_MANAGED", pin, value, &readback);
    if (controller_result == 0)
    {
        if (readback != value)
        {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    if (controller_result == -2)
    {
        fprintf(stderr,
                "%s 已设置为 %d，但后台保存统一状态失败：%s\n",
                pin, value, strerror(errno));
        return -1;
    }
    if (controller_result < 0 && (errno == EINVAL || errno == EPROTO))
    {
        /* 兼容旧后台：只有此路径仍需要客户端自行保存状态。 */
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
            "  %s [--pin GPIOx_Yz] input\n"
            "  %s [--pin GPIOx_Yz] output <0|1|low|high>\n"
            "  %s [--pin GPIOx_Yz] get\n"
            "  %s [--pin GPIOx_Yz] read\n"
            "  %s restore\n"
            "input 切换为输入；output 必须同时指定安全的初始电平。\n"
            "get 只查询框架输出/保存状态；read 只读取已处于输入方向的外部电平。\n"
            "未指定 --pin 时使用 %s；不带命令不会操作任何 GPIO。\n",
            program, program, program, program, program, program, DEFAULT_PIN);
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
    const char *command = NULL;
    int argument = 1;
    int value;
    int persist;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0))
    {
        usage(argv[0]);
        return 0;
    }
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
    if (!command)
    {
        usage(argv[0]);
        return 2;
    }

    persist = persistence_enabled();
    if (persist)
    {
        migrate_legacy_state("GPIO6_A0", LEGACY_GPIO_STATE);
        migrate_legacy_state("GPIO6_A2", LEGACY_RELAY_STATE);
    }

    if (strcmp(command, "input") == 0)
    {
        int mode_result;
        int managed_state = 0;

        if (argument != argc)
        {
            usage(argv[0]);
            return 2;
        }
        mode_result = set_line_input(pin, &managed_state);
        if (mode_result < 0)
        {
            if (errno == EBUSY)
                fprintf(stderr,
                        "无法将 %s 切换为输入：线路正被内核、其他程序或旧版 GPIO 后台占用。\n",
                        pin);
            else
                fprintf(stderr, "无法将 %s 切换为输入：%s\n", pin,
                        strerror(errno));
            return 1;
        }
        if (mode_result == 1)
            clear_runtime_output_record(pin);
        printf("%s 已切换为输入%s\n", pin,
               mode_result == 0 ? "（由 GPIO 控制服务持续保持）" :
                                  "（本地切换；未由后台持续占用）");
        if (persist && !managed_state)
        {
            if (clear_saved_output(pin) != 0)
            {
                fprintf(stderr, "警告：无法删除 %s 的旧输出保存值：%s\n",
                        pin, strerror(errno));
                fprintf(stderr,
                        "当前已是输入，但重启后仍可能被旧保存值恢复成输出。\n");
                return 1;
            }
            else
                printf("已清除该引脚的开机输出恢复值，重启时不会被恢复成输出。\n");
        }
        else
        {
            printf("提示：GPIO 电平保持服务已关闭，原有开机输出保存值未修改。\n");
        }
        return 0;
    }
    else if (strcmp(command, "output") == 0)
    {
        if (argument >= argc || parse_value(argv[argument], &value) != 0)
        {
            usage(argv[0]);
            return 2;
        }
        argument++;
        if (argument != argc || set_line(pin, value, persist) != 0)
            return 1;
        if (persist)
            printf("%s 已切换为输出并保存初始电平 %d\n", pin, value);
        else
            printf("%s 已切换为输出，初始电平 %d（保持服务已关闭，不保存）\n",
                   pin, value);
        return 0;
    }
    else if (strcmp(command, "get") == 0)
    {
        int saved_value;
        int has_saved_value;
        int runtime_value;
        int has_runtime_value;
        int status_result;
        int status_errno;

        if (argument != argc)
        {
            usage(argv[0]);
            return 2;
        }
        has_saved_value = load_value(pin, &saved_value) == 0;
        has_runtime_value = load_runtime_value(pin, &runtime_value) == 0;
        status_result = controller_request("STATUS", pin, 0, &value);
        status_errno = status_result < 0 ? errno : 0;
        if (status_result == 0)
        {
            if (has_saved_value)
                printf("%s 当前输出=%d，保存值=%d（保持服务%s）\n", pin,
                       value, saved_value, persist ? "已开启" : "已关闭");
            else
                printf("%s 当前输出=%d（没有保存值）\n", pin, value);
            return 0;
        }
        if (has_runtime_value)
        {
            if (status_result < 0 && status_errno == EINVAL && has_saved_value)
                printf("%s 当前输出=%d，保存值=%d（来源：旧后台运行记录；"
                       "查询未改变引脚）\n", pin, runtime_value, saved_value);
            else if (status_result < 0 && status_errno == EINVAL)
                printf("%s 当前输出=%d（来源：旧后台运行记录；没有保存值；"
                       "查询未改变引脚）\n", pin, runtime_value);
            else if (has_saved_value)
                printf("%s 最后运行记录=%d，保存值=%d（查询未改变引脚）\n",
                       pin, runtime_value, saved_value);
            else
                printf("%s 最后运行记录=%d（没有保存值，查询未改变引脚）\n",
                       pin, runtime_value);
            if (status_result == 1)
                printf("提示：GPIO 控制服务未运行，运行记录未验证为当前硬件状态。\n");
            else if (status_result < 0 && status_errno != EINVAL &&
                     status_errno != EPROTO && status_errno != ENOENT)
                printf("提示：后台状态查询失败（%s），以上为运行时文件记录。\n",
                       strerror(status_errno));
            return 0;
        }
        if (has_saved_value)
        {
            printf("%s 保存值=%d（%s，当前电平未验证，且本命令未改变引脚）\n",
                   pin, saved_value,
                   status_result == 1 ? "GPIO 控制服务未运行" :
                                        "当前未由控制服务保持输出");
            return 0;
        }
        if (status_result < 0 && status_errno != EINVAL &&
            status_errno != EPROTO && status_errno != ENOENT)
        {
            fprintf(stderr, "无法查询 %s 输出状态：%s\n", pin,
                    strerror(status_errno));
            return 1;
        }
        fprintf(stderr,
                "%s 没有框架输出状态或保存值；如需读取外部输入，请使用 read。\n",
                pin);
        return 1;
    }
    else if (strcmp(command, "read") == 0)
    {
        if (argument != argc)
        {
            usage(argv[0]);
            return 2;
        }
        if (read_line_as_input(pin, &value) != 0)
        {
            if (errno == EBUSY)
                fprintf(stderr,
                        "无法读取 %s：该线路不是空闲输入，可能正作为输出或被其他驱动占用。\n",
                        pin);
            else if (errno == EINVAL)
                fprintf(stderr,
                        "无法读取 %s：引脚名无效，或内核不支持该输入请求。\n",
                        pin);
            else
                fprintf(stderr, "无法读取 %s 输入电平：%s\n", pin,
                        strerror(errno));
            return 1;
        }
        printf("%s=%d（输入电平）\n", pin, value);
        return 0;
    }
    else
    {
        usage(argv[0]);
        return 2;
    }
}
