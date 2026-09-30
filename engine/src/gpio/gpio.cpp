/**
 * @file gpio.cpp
 * @brief 视觉主程序 GPIO 公共接口实现（libgpiod 1.x）
 */

#include "gpio/gpio.h"

#include <gpiod.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define GPIO_MAXGROUP_NUM 256
#define GPIO_CONSUMER "rk3588_gpio_state"
#ifndef GPIO_STATE_DIR
#define GPIO_STATE_DIR "/var/lib/rk3588-gpio"
#endif
#ifndef GPIO_RUNTIME_DIR
#define GPIO_RUNTIME_DIR "/run/rk3588-gpio-control"
#endif
#ifndef GPIO_PERSISTENCE_ENABLE_FILE
#define GPIO_PERSISTENCE_ENABLE_FILE "/run/rk3588-gpio-persistence/enabled"
#endif
#ifndef GPIO_CONTROL_SOCKET
#define GPIO_CONTROL_SOCKET "/run/rk3588-gpio-control/control.sock"
#endif
#define LEGACY_GPIO_STATE "/var/lib/gpio6_a0_test.state"
#define LEGACY_RELAY_STATE "/var/lib/relay_test.state"

namespace
{

struct GpioPin
{
    std::string name;
    int group = -1;
    int offset = -1;
    int direction = GPIO_DIRECTION_UNKNOWN;
    int defaultVal = 0;
    int persistedVal = -1;
    struct gpiod_line *line = nullptr;
    bool errLogged = false;
    bool persistErrLogged = false;
};

struct ControllerInfo
{
    bool isRk3588Bank = false;
    char identity[GPIO_TEXT_LONG] = {};
    char powerDomain[GPIO_TEXT_LONG] = {};
    char voltageDescription[GPIO_TEXT_LONG] = {};
    int minimumMicrovolts = 0;
    int maximumMicrovolts = 0;
    bool voltageExact = false;
};

std::mutex g_mutex;
std::vector<GpioPin> g_pins;
struct gpiod_chip *g_chips[GPIO_MAXGROUP_NUM] = {};
bool g_chipErrLogged[GPIO_MAXGROUP_NUM] = {};
std::set<std::string> g_badNames;

void migrate_legacy_state(const char *pinName, const char *legacyPath);

template <size_t N> void copy_text(char (&destination)[N], const char *source)
{
    if (!source)
        source = "";
    size_t count = strlen(source);
    if (count >= N)
        count = N - 1;
    memcpy(destination, source, count);
    destination[count] = '\0';
}

void copy_text(char *destination, size_t size, const char *source)
{
    if (!destination || size == 0)
        return;
    if (!source)
        source = "";
    size_t count = strlen(source);
    if (count >= size)
        count = size - 1;
    memcpy(destination, source, count);
    destination[count] = '\0';
}

bool persistence_enabled()
{
    return access(GPIO_PERSISTENCE_ENABLE_FILE, F_OK) == 0;
}

bool parse_pin_name(const char *pinName, int *group, int *offset)
{
    unsigned int parsedGroup = 0;
    unsigned int bit = 0;
    char bank = '\0';
    int consumed = 0;

    if (!pinName || !group || !offset || sscanf(pinName, "GPIO%u_%c%u%n", &parsedGroup, &bank, &bit, &consumed) != 3 ||
        pinName[consumed] != '\0' || parsedGroup >= GPIO_MAXGROUP_NUM || bank < 'A' || bank > 'Z' || bit > 7)
    {
        errno = EINVAL;
        return false;
    }
    *group = static_cast<int>(parsedGroup);
    *offset = 8 * (bank - 'A') + static_cast<int>(bit);
    return true;
}

void log_bad_name_once(const char *pinName)
{
    if (pinName && g_badNames.insert(pinName).second)
        printf("【GPIO】引脚名 \"%s\" 非法，应为 GPIOx_Yz 格式（如 GPIO6_A0）\n", pinName);
}

/*
 * 返回 0=后台完整处理成功，1=后台未运行，-1=后台未执行，
 * -2=GPIO 已经改变、但后台未能完成状态落盘。
 * 新操作在旧后台上通常返回 EINVAL，调用方可据此选择安全的本地回退。
 */
int controller_request(const char *operation, const char *pinName, int requestedValue, int *returnedValue)
{
    struct sockaddr_un address{};
    char request[128] = {};
    char response[256] = {};
    int socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socketFd < 0)
        return 1;

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", GPIO_CONTROL_SOCKET);
    if (connect(socketFd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0)
    {
        int connectErrno = errno;
        close(socketFd);
        if (connectErrno == ENOENT || connectErrno == ECONNREFUSED)
            return 1;
        errno = connectErrno;
        return -1;
    }

    int requestSize = -1;
    if (strcmp(operation, "SET") == 0 || strcmp(operation, "SET_MANAGED") == 0)
    {
        requestSize = snprintf(request, sizeof(request), "%s %s %d", operation, pinName,
                               requestedValue ? 1 : 0);
    }
    else if (strcmp(operation, "INPUT") == 0 || strcmp(operation, "INPUT_MANAGED") == 0)
    {
        requestSize = snprintf(request, sizeof(request), "%s %s", operation, pinName);
    }
    else if (strcmp(operation, "GET") == 0 || strcmp(operation, "READ") == 0 || strcmp(operation, "STATUS") == 0)
    {
        requestSize = snprintf(request, sizeof(request), "%s %s", operation, pinName);
    }
    else
    {
        close(socketFd);
        errno = EINVAL;
        return -1;
    }

    if (requestSize <= 0 || requestSize >= static_cast<int>(sizeof(request)) ||
        send(socketFd, request, static_cast<size_t>(requestSize), MSG_NOSIGNAL) != requestSize)
    {
        int sendErrno = errno;
        close(socketFd);
        errno = sendErrno;
        return -1;
    }

    ssize_t responseSize = recv(socketFd, response, sizeof(response) - 1, 0);
    close(socketFd);
    if (responseSize <= 0)
    {
        errno = EIO;
        return -1;
    }
    response[responseSize] = '\0';

    int value = 0;
    if (sscanf(response, "OK %d", &value) == 1)
    {
        if (returnedValue)
            *returnedValue = value;
        return 0;
    }
    int appliedValue = 0;
    int appliedErrno = EIO;
    if (sscanf(response, "APPLIED %d %d", &appliedValue, &appliedErrno) == 2)
    {
        if (returnedValue)
            *returnedValue = appliedValue;
        errno = appliedErrno > 0 ? appliedErrno : EIO;
        return -2;
    }
    int remoteErrno = EPROTO;
    if (sscanf(response, "ERR %d", &remoteErrno) != 1)
        remoteErrno = EPROTO;
    errno = remoteErrno;
    return -1;
}

bool state_path(const char *pinName, char *path, size_t size)
{
    int count = snprintf(path, size, "%s/%s.state", GPIO_STATE_DIR, pinName);
    if (count <= 0 || static_cast<size_t>(count) >= size)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool runtime_level_path(const char *pinName, char *path, size_t size)
{
    int count = snprintf(path, size, "%s/%s.level", GPIO_RUNTIME_DIR, pinName);
    if (count <= 0 || static_cast<size_t>(count) >= size)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool ensure_state_dir()
{
    struct stat info{};
    if (mkdir(GPIO_STATE_DIR, 0755) != 0 && errno != EEXIST)
        return false;
    if (lstat(GPIO_STATE_DIR, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0022) != 0)
    {
        errno = EPERM;
        return false;
    }
    return true;
}

bool read_value_file(const char *path, int *value)
{
    char text[8] = {};
    struct stat info{};
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    ssize_t count = read(fd, text, sizeof(text) - 1);
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
        count = -1;
    close(fd);
    if (count < 1 || (text[0] != '0' && text[0] != '1') || (count > 1 && text[1] != '\n' && text[1] != '\0'))
    {
        errno = EINVAL;
        return false;
    }
    *value = text[0] - '0';
    return true;
}

bool load_saved_value(const char *pinName, int *value)
{
    char path[PATH_MAX];
    return value && state_path(pinName, path, sizeof(path)) && read_value_file(path, value);
}

bool load_runtime_value(const char *pinName, int *value)
{
    char path[PATH_MAX];
    return value && runtime_level_path(pinName, path, sizeof(path)) && read_value_file(path, value);
}

bool save_persistent_value(const char *pinName, int value)
{
    char finalPath[PATH_MAX];
    char temporaryPath[PATH_MAX];
    char text[2] = {static_cast<char>('0' + (value ? 1 : 0)), '\n'};

    if (!ensure_state_dir() || !state_path(pinName, finalPath, sizeof(finalPath)) ||
        snprintf(temporaryPath, sizeof(temporaryPath), "%s/.%s.%ld.tmp", GPIO_STATE_DIR, pinName,
                 static_cast<long>(getpid())) >= static_cast<int>(sizeof(temporaryPath)))
        return false;

    unlink(temporaryPath);
    int fd = open(temporaryPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    bool ok = write(fd, text, sizeof(text)) == static_cast<ssize_t>(sizeof(text));
    if (ok)
        ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok)
        ok = rename(temporaryPath, finalPath) == 0;
    if (ok)
    {
        int directoryFd = open(GPIO_STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ok = directoryFd >= 0 && fsync(directoryFd) == 0;
        if (directoryFd >= 0)
            close(directoryFd);
    }
    if (!ok)
        unlink(temporaryPath);
    return ok;
}

bool unlink_if_exists(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT;
}

bool clear_saved_output(const char *pinName)
{
    char path[PATH_MAX];
    if (!state_path(pinName, path, sizeof(path)) || !unlink_if_exists(path))
        return false;
    if (strcmp(pinName, "GPIO6_A0") == 0 && !unlink_if_exists(LEGACY_GPIO_STATE))
        return false;
    if (strcmp(pinName, "GPIO6_A2") == 0 && !unlink_if_exists(LEGACY_RELAY_STATE))
        return false;
    return true;
}

void clear_runtime_output_record(const char *pinName)
{
    char path[PATH_MAX];
    if (runtime_level_path(pinName, path, sizeof(path)))
        (void)unlink(path);
}

bool persist_output(GpioPin *pin, int value)
{
    value = value ? 1 : 0;
    if (!persistence_enabled())
    {
        pin->persistedVal = -1;
        pin->persistErrLogged = false;
        return true;
    }
    /*
     * 不以进程内缓存跳过写入：旧后台/本地回退期间，命令行工具可能已经从
     * 另一个进程更新同一状态文件，缓存不能再被视为持久状态的事实来源。
     */
    if (!save_persistent_value(pin->name.c_str(), value))
    {
        if (!pin->persistErrLogged)
        {
            printf("【GPIO】保存 %s=%d 到 %s 失败：%s\n", pin->name.c_str(), value, GPIO_STATE_DIR, strerror(errno));
            pin->persistErrLogged = true;
        }
        return false;
    }
    pin->persistedVal = value;
    pin->persistErrLogged = false;
    return true;
}

struct gpiod_chip *get_chip(int group)
{
    if (group < 0 || group >= GPIO_MAXGROUP_NUM)
    {
        errno = EINVAL;
        return nullptr;
    }
    if (!g_chips[group])
    {
        char chipName[32];
        snprintf(chipName, sizeof(chipName), "gpiochip%d", group);
        g_chips[group] = gpiod_chip_open_by_name(chipName);
        if (!g_chips[group])
        {
            if (!g_chipErrLogged[group])
            {
                printf("【GPIO】打开 %s 失败：%s\n", chipName, strerror(errno));
                g_chipErrLogged[group] = true;
            }
        }
        else
        {
            g_chipErrLogged[group] = false;
        }
    }
    return g_chips[group];
}

int find_pin(const char *pinName)
{
    for (size_t i = 0; i < g_pins.size(); ++i)
        if (g_pins[i].name == pinName)
            return static_cast<int>(i);
    return -1;
}

void log_pin_error_once(GpioPin *pin, const char *format, ...)
{
    if (!pin || pin->errLogged)
        return;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    pin->errLogged = true;
}

void release_pin_line(GpioPin *pin)
{
    if (pin && pin->line)
    {
        gpiod_line_release(pin->line);
        pin->line = nullptr;
    }
}

struct gpiod_line *request_line(GpioPin *pin, int direction, int value)
{
    struct gpiod_chip *chip = get_chip(pin->group);
    if (!chip)
    {
        log_pin_error_once(pin, "【GPIO】打开 %s 所在芯片失败\n", pin->name.c_str());
        return nullptr;
    }

    struct gpiod_line *line = gpiod_chip_get_line(chip, pin->offset);
    if (!line)
    {
        log_pin_error_once(pin, "【GPIO】获取 %s 失败（组%d 线%d）\n", pin->name.c_str(), pin->group, pin->offset);
        return nullptr;
    }

    int result = direction == DIR_OUTPUT ? gpiod_line_request_output(line, GPIO_CONSUMER, value ? 1 : 0)
                                         : gpiod_line_request_input(line, GPIO_CONSUMER);
    if (result < 0)
    {
        log_pin_error_once(pin, "【GPIO】请求 %s 为%s失败：%s\n", pin->name.c_str(),
                           direction == DIR_OUTPUT ? "输出" : "输入", strerror(errno));
        gpiod_line_release(line);
        return nullptr;
    }
    return line;
}

GpioPin *find_or_add_pin(const char *pinName, int group, int offset, int direction)
{
    int index = find_pin(pinName);
    if (index >= 0)
        return &g_pins[static_cast<size_t>(index)];

    GpioPin pin;
    pin.name = pinName;
    pin.group = group;
    pin.offset = offset;
    pin.direction = direction;
    g_pins.push_back(pin);
    printf("【GPIO】%s 未在本进程中预配置，现按需加入本进程管理\n", pinName);
    return &g_pins.back();
}

int configure_output_locked(GpioPin *pin, int value)
{
    value = value ? 1 : 0;

    /* 后台不可用期间由本进程持有的线路直接原地切换，避免自我抢占。 */
    if (pin->line)
    {
        int result = pin->direction == DIR_INPUT ? gpiod_line_set_direction_output(pin->line, value)
                                                 : gpiod_line_set_value(pin->line, value);
        if (result < 0)
        {
            log_pin_error_once(pin, "【GPIO】将 %s 切换为输出失败：%s\n", pin->name.c_str(), strerror(errno));
            return -2;
        }
        pin->direction = DIR_OUTPUT;
        pin->defaultVal = value;
        if (!persist_output(pin, value))
            return -3;
        pin->errLogged = false;
        return 0;
    }

    int readback = -1;
    int controllerResult = controller_request("SET_MANAGED", pin->name.c_str(), value, &readback);
    if (controllerResult == 0)
    {
        if (readback != value)
        {
            errno = EIO;
            log_pin_error_once(pin, "【GPIO】%s 输出校验失败（期望%d，读取%d）\n", pin->name.c_str(), value, readback);
            return -2;
        }
        pin->direction = DIR_OUTPUT;
        pin->defaultVal = value;
        pin->persistedVal = persistence_enabled() ? value : -1;
        pin->errLogged = false;
        return 0;
    }
    if (controllerResult == -2)
    {
        pin->direction = DIR_OUTPUT;
        pin->defaultVal = value;
        log_pin_error_once(pin,
                           "【GPIO】%s 已输出 %d，但后台保存统一状态失败：%s\n",
                           pin->name.c_str(), value, strerror(errno));
        return -3;
    }
    if (controllerResult < 0 && (errno == EINVAL || errno == EPROTO))
    {
        /* 兼容尚未支持统一持久化协议的旧后台。 */
        controllerResult = controller_request("SET", pin->name.c_str(), value, &readback);
        if (controllerResult == 0)
        {
            if (readback != value)
            {
                errno = EIO;
                log_pin_error_once(pin, "【GPIO】%s 输出校验失败（期望%d，读取%d）\n",
                                   pin->name.c_str(), value, readback);
                return -2;
            }
            pin->direction = DIR_OUTPUT;
            pin->defaultVal = value;
            if (!persist_output(pin, value))
                return -3;
            pin->errLogged = false;
            return 0;
        }
    }
    if (controllerResult < 0)
    {
        log_pin_error_once(pin, "【GPIO】实时控制服务无法设置 %s：%s\n", pin->name.c_str(), strerror(errno));
        return -2;
    }

    pin->line = request_line(pin, DIR_OUTPUT, value);
    if (!pin->line)
        return -2;
    pin->direction = DIR_OUTPUT;
    pin->defaultVal = value;
    if (!persist_output(pin, value))
        return -3;
    pin->errLogged = false;
    return 0;
}

int configure_input_locked(GpioPin *pin)
{
    if (pin->line)
    {
        if (pin->direction != DIR_INPUT && gpiod_line_set_direction_input(pin->line) != 0)
        {
            log_pin_error_once(pin, "【GPIO】将 %s 切换为输入失败：%s\n", pin->name.c_str(), strerror(errno));
            return -2;
        }
        pin->direction = DIR_INPUT;
    }
    else
    {
        bool managedState = false;
        int controllerResult = controller_request("INPUT_MANAGED", pin->name.c_str(), 0, nullptr);
        if (controllerResult == 0)
        {
            pin->direction = DIR_INPUT;
            pin->persistedVal = -1;
            managedState = true;
        }
        else if (controllerResult == -2)
        {
            pin->direction = DIR_INPUT;
            log_pin_error_once(pin,
                               "【GPIO】%s 已切换为输入，但后台清除统一状态失败：%s\n",
                               pin->name.c_str(), strerror(errno));
            return -3;
        }
        else if (controllerResult < 0 && (errno == EINVAL || errno == EPROTO))
        {
            /* 旧后台只支持 INPUT，状态文件仍由兼容路径维护。 */
            controllerResult = controller_request("INPUT", pin->name.c_str(), 0, nullptr);
            if (controllerResult == 0)
                pin->direction = DIR_INPUT;
        }
        if (controllerResult > 0 || (controllerResult < 0 && (errno == EINVAL || errno == EPROTO)))
        {
            /* 兼容后台未运行或不支持 INPUT 的旧版本。 */
            pin->line = request_line(pin, DIR_INPUT, 0);
            if (!pin->line)
                return -2;
            pin->direction = DIR_INPUT;
        }
        else if (controllerResult < 0)
        {
            log_pin_error_once(pin, "【GPIO】实时控制服务无法将 %s 设为输入：%s\n", pin->name.c_str(), strerror(errno));
            return -2;
        }

        if (managedState)
        {
            pin->errLogged = false;
            return 0;
        }
    }

    clear_runtime_output_record(pin->name.c_str());
    if (persistence_enabled())
    {
        if (!clear_saved_output(pin->name.c_str()))
        {
            log_pin_error_once(pin, "【GPIO】%s 已切换为输入，但清除输出恢复值失败：%s\n", pin->name.c_str(),
                               strerror(errno));
            return -3;
        }
        pin->persistedVal = -1;
    }
    pin->errLogged = false;
    return 0;
}

int read_local_input_locked(GpioPin *pin, int *value)
{
    if (!pin || !value)
    {
        errno = EINVAL;
        return -1;
    }
    if (pin->direction != DIR_INPUT)
    {
        errno = EBUSY;
        return -1;
    }
    if (!pin->line)
    {
        errno = ENOENT;
        return -1;
    }
    int result = gpiod_line_get_value(pin->line);
    if (result < 0)
        return -1;
    *value = result;
    pin->errLogged = false;
    return 0;
}

int strict_read_fallback(const char *pinName, int group, int offset, int *value)
{
    char chipName[32];
    snprintf(chipName, sizeof(chipName), "gpiochip%d", group);
    struct gpiod_chip *chip = gpiod_chip_open_by_name(chipName);
    if (!chip)
        return -1;
    struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
    if (!line || gpiod_line_direction(line) != GPIOD_LINE_DIRECTION_INPUT)
    {
        gpiod_chip_close(chip);
        errno = EBUSY;
        return -1;
    }
    if (gpiod_line_request_input(line, GPIO_CONSUMER) != 0)
    {
        gpiod_chip_close(chip);
        return -1;
    }
    int result = gpiod_line_get_value(line);
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    if (result < 0)
        return -1;
    *value = result;
    (void)pinName;
    return 0;
}

} // namespace

/* ======================== 方向与电平控制 ======================== */

int gpio_init(const GPIOCfg_t cfg[], int size)
{
    if (!cfg || size <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (persistence_enabled())
    {
        migrate_legacy_state("GPIO6_A0", LEGACY_GPIO_STATE);
        migrate_legacy_state("GPIO6_A2", LEGACY_RELAY_STATE);
    }
    for (auto &pin : g_pins)
        release_pin_line(&pin);
    g_pins.clear();
    g_badNames.clear();

    printf(">>>>> GPIO 预注册 %d 个引脚：\n", size);
    for (int i = 0; i < size; ++i)
    {
        int group = 0;
        int offset = 0;
        if (!parse_pin_name(cfg[i].pinName, &group, &offset) ||
            (cfg[i].direction != DIR_INPUT && cfg[i].direction != DIR_OUTPUT))
        {
            printf("[%d]======(Name:%s) 【配置非法，已跳过】\n", i, cfg[i].pinName);
            continue;
        }

        int oldIndex = find_pin(cfg[i].pinName);
        if (oldIndex >= 0)
        {
            release_pin_line(&g_pins[static_cast<size_t>(oldIndex)]);
            g_pins.erase(g_pins.begin() + oldIndex);
        }

        GpioPin pin;
        pin.name = cfg[i].pinName;
        pin.group = group;
        pin.offset = offset;
        pin.direction = cfg[i].direction;
        pin.defaultVal = cfg[i].val ? 1 : 0;
        if (pin.direction == DIR_OUTPUT)
        {
            int savedValue = 0;
            if (persistence_enabled() && load_saved_value(pin.name.c_str(), &savedValue))
            {
                pin.defaultVal = savedValue;
                pin.persistedVal = savedValue;
            }
        }
        g_pins.push_back(pin);
        GpioPin *registered = &g_pins.back();
        int result = registered->direction == DIR_OUTPUT ? configure_output_locked(registered, registered->defaultVal)
                                                         : configure_input_locked(registered);
        printf("[%d]======(Name:%s, Dir:%s) %s\n", i, cfg[i].pinName,
               cfg[i].direction == DIR_OUTPUT ? "OUTPUT" : "INPUT",
               result == 0 ? "【初始化成功】" : "【初始化失败，后续调用会自动重试】");
    }
    printf("-------------------------------------------------------------\n");
    return 0;
}

void gpio_deinit(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &pin : g_pins)
        release_pin_line(&pin);
    g_pins.clear();
    g_badNames.clear();
    for (int group = 0; group < GPIO_MAXGROUP_NUM; ++group)
    {
        if (g_chips[group])
        {
            gpiod_chip_close(g_chips[group]);
            g_chips[group] = nullptr;
        }
        g_chipErrLogged[group] = false;
    }
}

int gpio_set_output(const char *pinName, int initialValue)
{
    if (!pinName)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
    {
        log_bad_name_once(pinName);
        return -1;
    }
    GpioPin *pin = find_or_add_pin(pinName, group, offset, DIR_OUTPUT);
    return configure_output_locked(pin, initialValue);
}

int gpio_set_input(const char *pinName)
{
    if (!pinName)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
    {
        log_bad_name_once(pinName);
        return -1;
    }
    GpioPin *pin = find_or_add_pin(pinName, group, offset, DIR_INPUT);
    return configure_input_locked(pin);
}

int gpio_read_input(const char *pinName, int *value)
{
    if (!pinName || !value)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
    {
        log_bad_name_once(pinName);
        return -1;
    }

    int index = find_pin(pinName);
    if (index >= 0)
    {
        GpioPin *pin = &g_pins[static_cast<size_t>(index)];
        if (pin->line)
            return read_local_input_locked(pin, value);
        if (pin->direction == DIR_OUTPUT)
        {
            errno = EBUSY;
            return -1;
        }
    }

    int controllerResult = controller_request("READ", pinName, 0, value);
    if (controllerResult == 0)
        return 0;
    if (controllerResult < 0 && errno != EINVAL && errno != EPROTO)
        return -1;
    return strict_read_fallback(pinName, group, offset, value);
}

int pin_out_val(const char *pinName, int val)
{
    return gpio_set_output(pinName, val);
}

int pin_set_high(const char *pinName)
{
    return gpio_set_output(pinName, 1);
}

int pin_set_low(const char *pinName)
{
    return gpio_set_output(pinName, 0);
}

int read_pin_val(const char *pinName)
{
    if (!pinName)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
    {
        log_bad_name_once(pinName);
        return -1;
    }

    int index = find_pin(pinName);
    if (index >= 0)
    {
        GpioPin *pin = &g_pins[static_cast<size_t>(index)];
        if (pin->line)
        {
            int result = gpiod_line_get_value(pin->line);
            if (result < 0)
                log_pin_error_once(pin, "【GPIO】读取 %s 电平失败：%s\n", pinName, strerror(errno));
            else
                pin->errLogged = false;
            return result;
        }
    }

    int value = -1;
    int controllerResult = controller_request("GET", pinName, 0, &value);
    if (controllerResult == 0)
        return value;
    if (controllerResult < 0)
        return -1;

    GpioPin *pin =
        index >= 0 ? &g_pins[static_cast<size_t>(index)] : find_or_add_pin(pinName, group, offset, DIR_INPUT);
    if (!pin->line)
        pin->line = request_line(pin, pin->direction, pin->defaultVal);
    if (!pin->line)
        return -1;
    value = gpiod_line_get_value(pin->line);
    if (value < 0)
        log_pin_error_once(pin, "【GPIO】读取 %s 电平失败：%s\n", pinName, strerror(errno));
    else
        pin->errLogged = false;
    return value;
}

/* ======================== 状态与持久化 ======================== */

int gpio_get_output_status(const char *pinName, GPIOOutputStatus_t *status)
{
    if (!pinName || !status)
    {
        errno = EINVAL;
        return -1;
    }
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
        return -1;
    (void)group;
    (void)offset;

    memset(status, 0, sizeof(*status));
    status->persistenceEnabled = persistence_enabled() ? 1 : 0;
    if (status->persistenceEnabled)
    {
        if (strcmp(pinName, "GPIO6_A0") == 0)
            migrate_legacy_state(pinName, LEGACY_GPIO_STATE);
        else if (strcmp(pinName, "GPIO6_A2") == 0)
            migrate_legacy_state(pinName, LEGACY_RELAY_STATE);
    }
    status->hasSavedValue = load_saved_value(pinName, &status->savedValue) ? 1 : 0;
    status->hasRuntimeRecord = load_runtime_value(pinName, &status->runtimeValue) ? 1 : 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int index = find_pin(pinName);
        if (index >= 0)
        {
            GpioPin *pin = &g_pins[static_cast<size_t>(index)];
            if (pin->line && pin->direction == DIR_OUTPUT)
            {
                int localValue = gpiod_line_get_value(pin->line);
                if (localValue >= 0)
                {
                    status->hasCurrentValue = 1;
                    status->currentValue = localValue;
                    return 0;
                }
            }
        }
    }

    int value = 0;
    int controllerResult = controller_request("STATUS", pinName, 0, &value);
    int controllerErrno = controllerResult < 0 ? errno : 0;
    if (controllerResult == 0)
    {
        status->hasCurrentValue = 1;
        status->currentValue = value;
        return 0;
    }
    if (status->hasRuntimeRecord || status->hasSavedValue)
        return 0;
    if (controllerResult < 0 && controllerErrno != EINVAL && controllerErrno != EPROTO && controllerErrno != ENOENT)
    {
        errno = controllerErrno;
        return -1;
    }
    errno = ENOENT;
    return -1;
}

namespace
{

void migrate_legacy_state(const char *pinName, const char *legacyPath)
{
    int value = 0;
    if (load_saved_value(pinName, &value))
        return;
    if (read_value_file(legacyPath, &value))
        (void)save_persistent_value(pinName, value);
}

bool wait_for_gpio_chip(const char *pinName, int timeoutMilliseconds)
{
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
        return false;
    (void)offset;

    char devicePath[64];
    snprintf(devicePath, sizeof(devicePath), "/dev/gpiochip%d", group);
    struct timespec delay = {0, 20 * 1000 * 1000};
    for (int elapsed = 0; access(devicePath, F_OK) != 0; elapsed += 20)
    {
        if (elapsed >= timeoutMilliseconds)
        {
            errno = ENOENT;
            return false;
        }
        nanosleep(&delay, nullptr);
    }
    return true;
}

} // namespace

int gpio_restore_outputs(int *restoredCount, int *failedCount)
{
    int restored = 0;
    int failed = 0;
    if (restoredCount)
        *restoredCount = 0;
    if (failedCount)
        *failedCount = 0;
    if (!persistence_enabled())
        return 0;
    if (!ensure_state_dir())
        return -1;

    migrate_legacy_state("GPIO6_A0", LEGACY_GPIO_STATE);
    migrate_legacy_state("GPIO6_A2", LEGACY_RELAY_STATE);

    DIR *directory = opendir(GPIO_STATE_DIR);
    if (!directory)
        return -1;

    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr)
    {
        static const char suffix[] = ".state";
        size_t nameLength = strlen(entry->d_name);
        size_t suffixLength = sizeof(suffix) - 1;
        if (nameLength <= suffixLength || strcmp(entry->d_name + nameLength - suffixLength, suffix) != 0)
            continue;

        size_t pinLength = nameLength - suffixLength;
        char pinName[GPIO_TEXT_SHORT] = {};
        if (pinLength >= sizeof(pinName))
            continue;
        memcpy(pinName, entry->d_name, pinLength);

        int group = 0;
        int offset = 0;
        int value = 0;
        if (!parse_pin_name(pinName, &group, &offset))
            continue;
        (void)group;
        (void)offset;
        if (!load_saved_value(pinName, &value) || !wait_for_gpio_chip(pinName, 2000) ||
            gpio_set_output(pinName, value) != 0)
        {
            ++failed;
            continue;
        }
        ++restored;
    }
    closedir(directory);

    if (restoredCount)
        *restoredCount = restored;
    if (failedCount)
        *failedCount = failed;
    if (failed != 0)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

/* ======================== 枚举与标称电压 ======================== */

namespace
{

bool device_tree_has_compatible(const char *compatible)
{
    char buffer[1024];
    int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t count = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (count <= 0)
        return false;

    size_t offset = 0;
    while (offset < static_cast<size_t>(count))
    {
        size_t remaining = static_cast<size_t>(count) - offset;
        size_t length = strnlen(buffer + offset, remaining);
        if (length == strlen(compatible) && memcmp(buffer + offset, compatible, length) == 0)
            return true;
        if (length == remaining)
            break;
        offset += length + 1;
    }
    return false;
}

bool join_path(char *destination, size_t destinationSize, const char *directory, const char *name)
{
    size_t directoryLength = strlen(directory);
    size_t nameLength = strlen(name);
    if (directoryLength + 1 + nameLength + 1 > destinationSize)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(destination, directory, directoryLength);
    destination[directoryLength] = '/';
    memcpy(destination + directoryLength + 1, name, nameLength + 1);
    return true;
}

bool read_first_property_string(const char *path, char *value, size_t valueSize)
{
    if (!value || valueSize == 0)
    {
        errno = EINVAL;
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t count = read(fd, value, valueSize - 1);
    close(fd);
    if (count <= 0)
        return false;
    value[count] = '\0';
    return true;
}

bool read_device_tree_u32(const char *path, uint32_t *value)
{
    unsigned char bytes[4];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t count = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (count != static_cast<ssize_t>(sizeof(bytes)))
        return false;
    *value = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
             (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
    return true;
}

bool has_suffix(const char *text, const char *suffix)
{
    size_t textLength = strlen(text);
    size_t suffixLength = strlen(suffix);
    return textLength >= suffixLength && strcmp(text + textLength - suffixLength, suffix) == 0;
}

bool find_device_tree_node_by_phandle(const char *directory, uint32_t wanted, char *nodePath, size_t nodePathSize,
                                      unsigned int depth)
{
    if (depth > 32)
        return false;
    const char *properties[] = {"phandle", "linux,phandle"};
    char path[PATH_MAX];
    for (const char *property : properties)
    {
        uint32_t value = 0;
        if (snprintf(path, sizeof(path), "%s/%s", directory, property) < static_cast<int>(sizeof(path)) &&
            read_device_tree_u32(path, &value) && value == wanted)
        {
            return snprintf(nodePath, nodePathSize, "%s", directory) < static_cast<int>(nodePathSize);
        }
    }

    DIR *stream = opendir(directory);
    if (!stream)
        return false;
    struct dirent *entry = nullptr;
    while ((entry = readdir(stream)) != nullptr)
    {
        struct stat info{};
        if (entry->d_name[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= static_cast<int>(sizeof(path)) ||
            lstat(path, &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        if (find_device_tree_node_by_phandle(path, wanted, nodePath, nodePathSize, depth + 1))
        {
            closedir(stream);
            return true;
        }
    }
    closedir(stream);
    return false;
}

bool find_supply_property(const char *ofNodePath, char *propertyPath, size_t propertyPathSize, char *supplyName,
                          size_t supplyNameSize)
{
    char firstProperty[NAME_MAX + 1] = {};
    DIR *stream = opendir(ofNodePath);
    if (!stream)
        return false;
    struct dirent *entry = nullptr;
    while ((entry = readdir(stream)) != nullptr)
    {
        if (!has_suffix(entry->d_name, "-supply"))
            continue;
        if (strcmp(entry->d_name, "vcc-supply") == 0)
        {
            copy_text(firstProperty, sizeof(firstProperty), entry->d_name);
            break;
        }
        if (firstProperty[0] == '\0')
            copy_text(firstProperty, sizeof(firstProperty), entry->d_name);
    }
    closedir(stream);
    if (firstProperty[0] == '\0' || snprintf(propertyPath, propertyPathSize, "%s/%s", ofNodePath, firstProperty) >=
                                        static_cast<int>(propertyPathSize))
        return false;

    snprintf(supplyName, supplyNameSize, "%.*s", static_cast<int>(strlen(firstProperty) - strlen("-supply")),
             firstProperty);
    return true;
}

bool find_gpiochip_driver(const char *sysfsLink, char *driver, size_t driverSize)
{
    char current[PATH_MAX];
    if (!realpath(sysfsLink, current))
        return false;
    while (strncmp(current, "/sys/devices/", strlen("/sys/devices/")) == 0)
    {
        char driverLink[PATH_MAX];
        char resolved[PATH_MAX];
        if (snprintf(driverLink, sizeof(driverLink), "%s/driver", current) < static_cast<int>(sizeof(driverLink)) &&
            realpath(driverLink, resolved))
        {
            const char *name = strrchr(resolved, '/');
            name = name ? name + 1 : resolved;
            if (strcmp(name, "gpio_stub_drv") != 0)
            {
                copy_text(driver, driverSize, name);
                return true;
            }
        }
        char *slash = strrchr(current, '/');
        if (!slash)
            break;
        *slash = '\0';
    }
    return false;
}

void friendly_compatible_name(const char *compatible, char *name, size_t size)
{
    const char *model = strrchr(compatible, ',');
    model = model ? model + 1 : compatible;
    copy_text(name, size, model);
    if (strncmp(model, "pca", 3) == 0 || strncmp(model, "tca", 3) == 0 || strncmp(model, "mcp", 3) == 0)
    {
        for (size_t i = 0; name[i] != '\0'; ++i)
            name[i] = static_cast<char>(toupper(static_cast<unsigned char>(name[i])));
    }
}

bool rk3588_gpio_bank(const char *chipLabel, unsigned int *bank)
{
    unsigned int parsedBank = 0;
    int consumed = 0;
    if (!chipLabel || sscanf(chipLabel, "gpio%u%n", &parsedBank, &consumed) != 1 || chipLabel[consumed] != '\0' ||
        parsedBank > 4)
        return false;
    *bank = parsedBank;
    return true;
}

void describe_gpio_controller(unsigned int chipNumber, const char *chipLabel, bool isRk3588, ControllerInfo *controller)
{
    char devicePath[64];
    char sysfsPath[PATH_MAX];
    char propertyPath[PATH_MAX];
    char compatible[GPIO_TEXT_LONG] = {};
    char driver[64] = {};
    char model[64] = {};
    char supplyProperty[PATH_MAX];
    char supplyName[64] = {};
    char regulatorNode[PATH_MAX];
    char regulatorName[GPIO_TEXT_LONG] = {};
    uint32_t supplyPhandle = 0;
    uint32_t minimumUv = 0;
    uint32_t maximumUv = 0;
    struct stat deviceInfo{};
    unsigned int bank = 0;

    *controller = ControllerInfo{};
    snprintf(devicePath, sizeof(devicePath), "/dev/gpiochip%u", chipNumber);
    if (stat(devicePath, &deviceInfo) == 0 && S_ISCHR(deviceInfo.st_mode))
    {
        snprintf(sysfsPath, sizeof(sysfsPath), "/sys/dev/char/%u:%u", major(deviceInfo.st_rdev),
                 minor(deviceInfo.st_rdev));
    }
    else
    {
        snprintf(sysfsPath, sizeof(sysfsPath), "/sys/dev/char/254:%u", chipNumber);
    }

    if (join_path(propertyPath, sizeof(propertyPath), sysfsPath, "of_node/compatible"))
    {
        (void)read_first_property_string(propertyPath, compatible, sizeof(compatible));
    }
    (void)find_gpiochip_driver(sysfsPath, driver, sizeof(driver));

    if (isRk3588 && (strcmp(compatible, "rockchip,gpio-bank") == 0 || strcmp(driver, "rockchip-gpio") == 0) &&
        rk3588_gpio_bank(chipLabel, &bank))
    {
        controller->isRk3588Bank = true;
        snprintf(controller->identity, sizeof(controller->identity), "rockchip,rk3588/gpio%u", bank);
        return;
    }

    if (compatible[0] != '\0')
    {
        copy_text(controller->identity, compatible);
        friendly_compatible_name(compatible, model, sizeof(model));
    }
    else if (driver[0] != '\0')
    {
        snprintf(controller->identity, sizeof(controller->identity), "driver:%s", driver);
        copy_text(model, sizeof(model), driver);
    }
    else
    {
        copy_text(controller->identity, "未识别");
        copy_text(model, sizeof(model), chipLabel && chipLabel[0] ? chipLabel : "未知控制器");
    }

    if (strstr(model, "rk806") || strstr(model, "RK806"))
    {
        copy_text(controller->powerDomain, "RK806 GPIO域");
        copy_text(controller->voltageDescription, "需查RK806电路*");
        return;
    }

    snprintf(controller->powerDomain, sizeof(controller->powerDomain), "%s供电", model);
    if (snprintf(propertyPath, sizeof(propertyPath), "%s/of_node", sysfsPath) >=
            static_cast<int>(sizeof(propertyPath)) ||
        !find_supply_property(propertyPath, supplyProperty, sizeof(supplyProperty), supplyName, sizeof(supplyName)))
    {
        copy_text(controller->voltageDescription, "供电未描述*");
        return;
    }

    if (!read_device_tree_u32(supplyProperty, &supplyPhandle) ||
        !find_device_tree_node_by_phandle("/sys/firmware/devicetree/base", supplyPhandle, regulatorNode,
                                          sizeof(regulatorNode), 0))
    {
        snprintf(controller->voltageDescription, sizeof(controller->voltageDescription), "%s供电，电压未知*",
                 supplyName);
        return;
    }

    if (join_path(propertyPath, sizeof(propertyPath), regulatorNode, "regulator-name"))
    {
        (void)read_first_property_string(propertyPath, regulatorName, sizeof(regulatorName));
    }
    if (regulatorName[0] != '\0')
    {
        copy_text(controller->powerDomain, model);
        size_t used = strlen(controller->powerDomain);
        if (used + 1 < sizeof(controller->powerDomain))
        {
            controller->powerDomain[used++] = '/';
            controller->powerDomain[used] = '\0';
            copy_text(controller->powerDomain + used, sizeof(controller->powerDomain) - used, regulatorName);
        }
    }

    if (join_path(propertyPath, sizeof(propertyPath), regulatorNode, "regulator-min-microvolt"))
        (void)read_device_tree_u32(propertyPath, &minimumUv);
    if (join_path(propertyPath, sizeof(propertyPath), regulatorNode, "regulator-max-microvolt"))
        (void)read_device_tree_u32(propertyPath, &maximumUv);

    controller->minimumMicrovolts = static_cast<int>(minimumUv);
    controller->maximumMicrovolts = static_cast<int>(maximumUv);
    if (minimumUv != 0 && minimumUv == maximumUv)
    {
        controller->voltageExact = true;
        snprintf(controller->voltageDescription, sizeof(controller->voltageDescription), "约%.2fV（设备树）",
                 minimumUv / 1000000.0);
    }
    else if (minimumUv != 0 && maximumUv != 0)
    {
        snprintf(controller->voltageDescription, sizeof(controller->voltageDescription), "%.2f-%.2fV范围*",
                 minimumUv / 1000000.0, maximumUv / 1000000.0);
    }
    else
    {
        snprintf(controller->voltageDescription, sizeof(controller->voltageDescription), "%s供电，电压未知*",
                 supplyName);
    }
}

void rk3588_voltage(unsigned int bank, unsigned int offset, GPIOVoltageInfo_t *voltage)
{
    memset(voltage, 0, sizeof(*voltage));
    voltage->isMeasured = 0;
    const char *domain = "未知";
    const char *description = "无法判断*";
    int minimumUv = 0;
    int maximumUv = 0;
    bool exact = false;

    auto fixed18 = [&]() {
        description = "约1.8V（固定）";
        minimumUv = 1800000;
        maximumUv = 1800000;
        exact = true;
    };
    auto selectable = [&]() {
        description = "1.8V或3.3V*";
        minimumUv = 1800000;
        maximumUv = 3300000;
        exact = false;
    };

    switch (bank)
    {
    case 0:
        if (offset <= 12)
        {
            domain = "PMUIO1";
            fixed18();
        }
        else
        {
            domain = "PMUIO2";
            selectable();
        }
        break;
    case 1:
        if (offset <= 15 || offset >= 30)
        {
            domain = "VCCIO4";
            selectable();
        }
        else
        {
            domain = "VCCIO1";
            fixed18();
        }
        break;
    case 2:
        if (offset <= 5 || offset >= 24)
        {
            domain = "EMMCIO";
            fixed18();
        }
        else
        {
            domain = "VCCIO3";
            fixed18();
        }
        break;
    case 3:
        domain = "VCCIO5";
        selectable();
        break;
    case 4:
        if (offset <= 17)
        {
            domain = "VCCIO6";
            selectable();
        }
        else if (offset <= 23)
        {
            domain = "VCCIO3";
            fixed18();
        }
        else
        {
            domain = "VCCIO2";
            selectable();
        }
        break;
    default:
        break;
    }

    copy_text(voltage->powerDomain, domain);
    copy_text(voltage->description, description);
    voltage->minimumMicrovolts = minimumUv;
    voltage->maximumMicrovolts = maximumUv;
    voltage->isExact = exact ? 1 : 0;
}

void fill_voltage(const ControllerInfo &controller, const char *chipLabel, unsigned int offset,
                  GPIOVoltageInfo_t *voltage)
{
    unsigned int bank = 0;
    if (controller.isRk3588Bank && rk3588_gpio_bank(chipLabel, &bank))
    {
        rk3588_voltage(bank, offset, voltage);
        return;
    }
    memset(voltage, 0, sizeof(*voltage));
    copy_text(voltage->powerDomain, controller.powerDomain);
    copy_text(voltage->description, controller.voltageDescription);
    voltage->minimumMicrovolts = controller.minimumMicrovolts;
    voltage->maximumMicrovolts = controller.maximumMicrovolts;
    voltage->isExact = controller.voltageExact ? 1 : 0;
    voltage->isMeasured = 0;
}

bool is_framework_consumer(const char *consumer)
{
    return consumer && (strcmp(consumer, "rk3588_gpio_control") == 0 || strcmp(consumer, GPIO_CONSUMER) == 0);
}

bool fill_pin_info(struct gpiod_chip *chip, unsigned int chipNumber, unsigned int offset,
                   const ControllerInfo &controller, GPIOPinInfo_t *info)
{
    if (!chip || !info)
        return false;
    struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
    if (!line)
        return false;

    const char *chipName = gpiod_chip_name(chip);
    const char *chipLabel = gpiod_chip_label(chip);
    const char *lineName = gpiod_line_name(line);
    const char *consumer = gpiod_line_consumer(line);
    memset(info, 0, sizeof(*info));
    if (offset / 8U < 26U)
    {
        snprintf(info->pinName, sizeof(info->pinName), "GPIO%u_%c%u", chipNumber, static_cast<char>('A' + offset / 8U),
                 offset % 8U);
    }
    else
    {
        snprintf(info->pinName, sizeof(info->pinName), "GPIO%u@%u", chipNumber, offset);
    }
    copy_text(info->chipName, chipName ? chipName : "");
    copy_text(info->chipLabel, chipLabel ? chipLabel : "");
    copy_text(info->controller, controller.identity);
    copy_text(info->lineName, lineName ? lineName : "");
    copy_text(info->consumer, consumer ? consumer : "");
    info->group = chipNumber;
    info->offset = offset;
    int direction = gpiod_line_direction(line);
    info->direction = direction == GPIOD_LINE_DIRECTION_OUTPUT  ? DIR_OUTPUT
                      : direction == GPIOD_LINE_DIRECTION_INPUT ? DIR_INPUT
                                                                : GPIO_DIRECTION_UNKNOWN;
    info->used = gpiod_line_is_used(line) ? 1 : 0;
    info->frameworkOwned = is_framework_consumer(consumer) ? 1 : 0;
    info->access = !info->used            ? GPIO_ACCESS_AVAILABLE
                   : info->frameworkOwned ? GPIO_ACCESS_FRAMEWORK
                                          : GPIO_ACCESS_BUSY;
    fill_voltage(controller, chipLabel, offset, &info->voltage);
    return true;
}

bool parse_chip_device_name(const char *name, unsigned int *number)
{
    int consumed = 0;
    return sscanf(name, "gpiochip%u%n", number, &consumed) == 1 && name[consumed] == '\0';
}

} // namespace

int gpio_get_pin_info(const char *pinName, GPIOPinInfo_t *info)
{
    if (!pinName || !info)
    {
        errno = EINVAL;
        return -1;
    }
    int group = 0;
    int offset = 0;
    if (!parse_pin_name(pinName, &group, &offset))
        return -1;

    struct gpiod_chip *chip = gpiod_chip_open_by_number(group);
    if (!chip)
        return -1;
    if (offset < 0 || static_cast<unsigned int>(offset) >= gpiod_chip_num_lines(chip))
    {
        gpiod_chip_close(chip);
        errno = ENOENT;
        return -1;
    }
    ControllerInfo controller;
    describe_gpio_controller(static_cast<unsigned int>(group), gpiod_chip_label(chip),
                             device_tree_has_compatible("rockchip,rk3588"), &controller);
    bool result =
        fill_pin_info(chip, static_cast<unsigned int>(group), static_cast<unsigned int>(offset), controller, info);
    gpiod_chip_close(chip);
    if (!result)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

int gpio_get_voltage_info(const char *pinName, GPIOVoltageInfo_t *info)
{
    if (!info)
    {
        errno = EINVAL;
        return -1;
    }
    GPIOPinInfo_t pinInfo{};
    if (gpio_get_pin_info(pinName, &pinInfo) != 0)
        return -1;
    *info = pinInfo.voltage;
    return 0;
}

int gpio_list_lines(const GPIOListOptions_t *options, GPIOPinInfo_t *items, size_t capacity, size_t *requiredCount)
{
    if (!requiredCount || (capacity != 0 && !items))
    {
        errno = EINVAL;
        return -1;
    }
    int chipFilter = options ? options->chip : -1;
    bool onlyAvailable = options && options->onlyAvailable;
    if (chipFilter < -1 || chipFilter >= GPIO_MAXGROUP_NUM)
    {
        errno = EINVAL;
        return -1;
    }

    std::vector<unsigned int> chips;
    DIR *directory = opendir("/dev");
    if (!directory)
        return -1;
    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr)
    {
        unsigned int number = 0;
        if (parse_chip_device_name(entry->d_name, &number) && number < GPIO_MAXGROUP_NUM)
            chips.push_back(number);
    }
    closedir(directory);
    std::sort(chips.begin(), chips.end());
    chips.erase(std::unique(chips.begin(), chips.end()), chips.end());
    if (chips.empty())
    {
        errno = ENODEV;
        return -1;
    }

    size_t count = 0;
    bool isRk3588 = device_tree_has_compatible("rockchip,rk3588");
    bool selectedChipFound = chipFilter < 0;
    for (unsigned int chipNumber : chips)
    {
        if (chipFilter >= 0 && chipNumber != static_cast<unsigned int>(chipFilter))
            continue;
        selectedChipFound = true;
        struct gpiod_chip *chip = gpiod_chip_open_by_number(chipNumber);
        if (!chip)
        {
            if (chipFilter >= 0)
                return -1;
            continue;
        }
        ControllerInfo controller;
        describe_gpio_controller(chipNumber, gpiod_chip_label(chip), isRk3588, &controller);
        unsigned int lineCount = gpiod_chip_num_lines(chip);
        for (unsigned int offset = 0; offset < lineCount; ++offset)
        {
            GPIOPinInfo_t current{};
            if (!fill_pin_info(chip, chipNumber, offset, controller, &current))
                continue;
            if (onlyAvailable && current.access == GPIO_ACCESS_BUSY)
                continue;
            if (count < capacity)
                items[count] = current;
            ++count;
        }
        gpiod_chip_close(chip);
    }
    if (!selectedChipFound)
    {
        errno = ENODEV;
        return -1;
    }
    *requiredCount = count;
    return 0;
}
