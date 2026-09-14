/**
 * @file gpio.cpp
 * @brief GPIO 底层控制封装实现（基于 libgpiod 1.x, 移植自 Easy EAI 08_GPIO 例程）
 *
 * 相比原例程的增强点:
 *   - 全局互斥锁: 通道逻辑多线程并行调用安全
 *   - 懒加载:     未预注册引脚首次调用时自动打开(按调用方向)
 *   - 自愈重试:   打开失败的引脚每次调用会重新尝试, 同类错误只打印一次
 *   - 芯片句柄按组惰性打开, 不再启动时遍历打开全部 7 个 chip
 */

#include "gpio/gpio.h"

#include <gpiod.h>

#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

/* 覆盖动态枚举的 gpiochip0 ~ gpiochip255，适配带不同扩展器的板卡。 */
#define GPIO_MAXGROUP_NUM 256
#define GPIO_STATE_DIR "/var/lib/rk3588-gpio"
#define GPIO_PERSISTENCE_ENABLE_FILE "/run/rk3588-gpio-persistence/enabled"
#define GPIO_CONTROL_SOCKET "/run/rk3588-gpio-control/control.sock"

namespace {

struct GpioPin {
    std::string name;
    int group = -1;   /* GPIO 组号(即 gpiochip 编号) */
    int offset = -1;  /* 组内线号 */
    int direction = DIR_OUTPUT; /* DIR_OUTPUT / DIR_INPUT */
    int defaultVal = 0;
    int persistedVal = -1; /* -1=没有已保存状态；0/1=磁盘中的期望电平 */
    struct gpiod_line *line = nullptr; /* gpiod 1.x 行句柄 */
    bool daemonManaged = false;        /* 由独立实时控制服务持有 */
    bool autoOpened = false;           /* 懒加载自动打开(true) / 预注册(false) */
    bool errLogged = false;            /* 同类错误只打印一次 */
    bool persistErrLogged = false;     /* 持久化错误只打印一次 */
};

std::mutex g_mutex;                                  /* 全部公共接口的互斥锁 */
std::vector<GpioPin> g_pins;                         /* 引脚表(含打开失败的空句柄项) */
struct gpiod_chip *g_chips[GPIO_MAXGROUP_NUM] = {};  /* 按组惰性打开的芯片句柄 */
bool g_chip_errLogged[GPIO_MAXGROUP_NUM] = {};       /* 芯片打开失败只打印一次 */
std::set<std::string> g_bad_names;                   /* 已报错的非法引脚名, 防逐帧刷屏 */

/*
 * GPIO 电平保持由独立的 systemd 服务统一开关。视觉应用只读取这个运行时标志，
 * 绝不自行启停服务：服务关闭时仍允许实时控制 GPIO，但不会读取或更新持久状态。
 */
bool persistence_enabled()
{
    return access(GPIO_PERSISTENCE_ENABLE_FILE, F_OK) == 0;
}

/* 返回 0=后台已处理，1=后台未运行（调用方可回退直控），-1=后台返回错误。 */
int controller_request(const char *operation, const char *pinName,
                       int requestedValue, int *returnedValue)
{
    struct sockaddr_un address {};
    char request[128] = {};
    char response[256] = {};
    int socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socketFd < 0)
        return 1;
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", GPIO_CONTROL_SOCKET);
    if (connect(socketFd, reinterpret_cast<struct sockaddr *>(&address),
                sizeof(address)) != 0) {
        int connectErrno = errno;
        close(socketFd);
        if (connectErrno == ENOENT || connectErrno == ECONNREFUSED)
            return 1;
        errno = connectErrno;
        return -1;
    }
    int requestSize = strcmp(operation, "SET") == 0
        ? snprintf(request, sizeof(request), "SET %s %d", pinName,
                   requestedValue ? 1 : 0)
        : snprintf(request, sizeof(request), "GET %s", pinName);
    if (requestSize <= 0 || requestSize >= static_cast<int>(sizeof(request)) ||
        send(socketFd, request, static_cast<size_t>(requestSize), MSG_NOSIGNAL) !=
            requestSize) {
        int sendErrno = errno;
        close(socketFd);
        errno = sendErrno;
        return -1;
    }
    ssize_t responseSize = recv(socketFd, response, sizeof(response) - 1, 0);
    close(socketFd);
    if (responseSize <= 0) {
        errno = EIO;
        return -1;
    }
    response[responseSize] = '\0';
    int value = 0;
    if (sscanf(response, "OK %d", &value) == 1) {
        if (returnedValue)
            *returnedValue = value;
        return 0;
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
    return count > 0 && static_cast<size_t>(count) < size;
}

bool ensure_state_dir()
{
    struct stat info {};
    if (mkdir(GPIO_STATE_DIR, 0755) != 0 && errno != EEXIST)
        return false;
    if (lstat(GPIO_STATE_DIR, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0022) != 0) {
        errno = EPERM;
        return false;
    }
    return true;
}

bool load_persistent_value(const char *pinName, int *value)
{
    char path[PATH_MAX];
    char text[8] = {};
    struct stat info {};
    if (!persistence_enabled() || !value ||
        !state_path(pinName, path, sizeof(path)))
        return false;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    ssize_t count = read(fd, text, sizeof(text) - 1);
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
        count = -1;
    close(fd);
    if (count < 1 || (text[0] != '0' && text[0] != '1') ||
        (count > 1 && text[1] != '\n' && text[1] != '\0'))
        return false;
    *value = text[0] - '0';
    return true;
}

bool save_persistent_value(const char *pinName, int value)
{
    char finalPath[PATH_MAX];
    char temporaryPath[PATH_MAX];
    char text[2] = {static_cast<char>('0' + (value ? 1 : 0)), '\n'};
    if (!ensure_state_dir() || !state_path(pinName, finalPath, sizeof(finalPath)) ||
        snprintf(temporaryPath, sizeof(temporaryPath), "%s/.%s.%ld.tmp",
                 GPIO_STATE_DIR, pinName, static_cast<long>(getpid())) >=
            static_cast<int>(sizeof(temporaryPath)))
        return false;

    unlink(temporaryPath);
    int fd = open(temporaryPath,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    bool ok = write(fd, text, sizeof(text)) == static_cast<ssize_t>(sizeof(text));
    if (ok)
        ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok)
        ok = rename(temporaryPath, finalPath) == 0;
    if (ok) {
        int directoryFd = open(GPIO_STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ok = directoryFd >= 0 && fsync(directoryFd) == 0;
        if (directoryFd >= 0)
            close(directoryFd);
    }
    if (!ok)
        unlink(temporaryPath);
    return ok;
}

bool persist_output(GpioPin *pin, int value)
{
    value = value ? 1 : 0;
    if (!persistence_enabled()) {
        /* 服务关闭期间的实时输出不能污染下次开机要恢复的状态。 */
        pin->persistedVal = -1;
        pin->persistErrLogged = false;
        return true;
    }
    if (pin->persistedVal == value)
        return true;
    if (!save_persistent_value(pin->name.c_str(), value)) {
        if (!pin->persistErrLogged) {
            printf("【GPIO】保存 %s=%d 到 %s 失败: %s\n",
                   pin->name.c_str(), value, GPIO_STATE_DIR, strerror(errno));
            pin->persistErrLogged = true;
        }
        return false;
    }
    pin->persistedVal = value;
    pin->persistErrLogged = false;
    return true;
}

/* 解析 "GPIOx_Yz": x=gpiochip编号, Y=bank字母A~Z, z=bank内编号0~7 */
bool parse_pin_name(const char *pinName, int *group, int *offset)
{
    unsigned int parsedGroup = 0;
    unsigned int bit = 0;
    char bank = '\0';
    int consumed = 0;
    if (!pinName ||
        sscanf(pinName, "GPIO%u_%c%u%n", &parsedGroup, &bank, &bit, &consumed) != 3 ||
        pinName[consumed] != '\0' || parsedGroup >= GPIO_MAXGROUP_NUM ||
        bank < 'A' || bank > 'Z' || bit > 7)
        return false;

    *group = static_cast<int>(parsedGroup);
    *offset = 8 * (bank - 'A') + static_cast<int>(bit);
    return true;
}

/* 按组号惰性打开 gpiochip, 失败返回 nullptr(错误只打印一次) */
struct gpiod_chip *get_chip(int group)
{
    if (group < 0 || group >= GPIO_MAXGROUP_NUM)
        return nullptr;
    if (!g_chips[group]) {
        char chipName[32];
        snprintf(chipName, sizeof(chipName), "gpiochip%d", group);
        g_chips[group] = gpiod_chip_open_by_name(chipName);
        if (!g_chips[group]) {
            if (!g_chip_errLogged[group]) {
                printf("【GPIO】打开 %s 失败\n", chipName);
                g_chip_errLogged[group] = true;
            }
        } else {
            g_chip_errLogged[group] = false;
        }
    }
    return g_chips[group];
}

/* 按名字查引脚表, 未找到返回 -1 */
int find_pin(const char *pinName)
{
    for (size_t i = 0; i < g_pins.size(); i++) {
        if (g_pins[i].name == pinName)
            return (int)i;
    }
    return -1;
}

/* 引脚操作统一错误打印: 同类错误只打印一次, 恢复成功后复位标记 */
void log_pin_error_once(GpioPin *pin, const char *fmt, ...)
{
    if (!pin || pin->errLogged)
        return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    pin->errLogged = true;
}

/* 请求一条线: 成功返回句柄; 失败返回 nullptr(错误按引脚缓存, 只打印一次) */
struct gpiod_line *request_line(GpioPin *pin, bool isOutput, int value)
{
    struct gpiod_chip *chip = get_chip(pin->group);
    if (!chip) {
        log_pin_error_once(pin, "【GPIO】打开 %s 所在芯片失败\n", pin->name.c_str());
        return nullptr;
    }

    struct gpiod_line *line = gpiod_chip_get_line(chip, pin->offset);
    if (!line) {
        log_pin_error_once(pin, "【GPIO】获取 %s 失败(组%d 线%d)\n", pin->name.c_str());
        return nullptr;
    }

    int ret = isOutput
                ? gpiod_line_request_output(line, pin->name.c_str(), value)
                : gpiod_line_request_input(line, pin->name.c_str());
    if (ret < 0) {
        log_pin_error_once(pin, "【GPIO】请求 %s 为%s失败(可能被占用或引脚不存在)\n",
                           pin->name.c_str(), isOutput ? "输出" : "输入");
        gpiod_line_release(line);
        return nullptr;
    }
    return line;
}

} // namespace

/* ======================== 对外接口 ======================== */

int gpio_init(const GPIOCfg_t cfg[], int size)
{
    if (!cfg || size <= 0)
        return -1;

    std::lock_guard<std::mutex> lock(g_mutex);

    /* 整体重建引脚表: 释放旧句柄, 按新配置重新打开 */
    for (auto &pin : g_pins) {
        if (pin.line) {
            gpiod_line_release(pin.line);
            pin.line = nullptr;
        }
    }
    g_pins.clear();
    g_bad_names.clear();

    printf(">>>>> GPIO 预注册 %d 个引脚:\n", size);
    for (int i = 0; i < size; i++) {
        int group = 0, offset = 0;
        if (!parse_pin_name(cfg[i].pinName, &group, &offset)) {
            printf("[%d]======(Name:%s)  【引脚名非法, 已跳过】\n", i, cfg[i].pinName);
            continue;
        }

        bool isOutput = (cfg[i].direction == DIR_OUTPUT);
        GpioPin pin;
        pin.name = cfg[i].pinName;
        pin.group = group;
        pin.offset = offset;
        pin.direction = cfg[i].direction;
        int initialValue = cfg[i].val ? 1 : 0;
        if (isOutput && load_persistent_value(pin.name.c_str(), &initialValue))
            pin.persistedVal = initialValue;
        pin.defaultVal = initialValue;
        pin.autoOpened = false;
        int controlledValue = -1;
        int controllerResult = isOutput
            ? controller_request("SET", pin.name.c_str(), initialValue, &controlledValue)
            : 1;
        if (controllerResult == 0) {
            pin.daemonManaged = true;
        } else if (controllerResult > 0) {
            pin.line = request_line(&pin, isOutput, initialValue);
        } else {
            log_pin_error_once(&pin, "【GPIO】实时控制服务无法设置 %s: %s\n",
                               pin.name.c_str(), strerror(errno));
        }
        if (isOutput && (pin.line || pin.daemonManaged))
            (void)persist_output(&pin, initialValue);

        /* 同名重复注册: 覆盖旧项 */
        int idx = find_pin(cfg[i].pinName);
        if (idx >= 0) {
            if (g_pins[idx].line) {
                gpiod_line_release(g_pins[idx].line);
                g_pins[idx].line = nullptr;
            }
            g_pins[idx] = pin;
        } else {
            g_pins.push_back(pin);
        }

        printf("[%d]======(Name:%s, Dir:%s)  %s\n", i, cfg[i].pinName,
               isOutput ? "OUTPUT" : "INPUT",
               (pin.line || pin.daemonManaged)
                   ? "【初始化成功】" : "【初始化失败, 后续调用会自动重试】");
    }
    printf("-------------------------------------------------------------\n");
    return 0;
}

void gpio_deinit(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto &pin : g_pins) {
        if (pin.line) {
            gpiod_line_release(pin.line);
            pin.line = nullptr;
        }
    }
    g_pins.clear();
    g_bad_names.clear();

    for (int i = 0; i < GPIO_MAXGROUP_NUM; i++) {
        if (g_chips[i]) {
            gpiod_chip_close(g_chips[i]);
            g_chips[i] = nullptr;
        }
        g_chip_errLogged[i] = false;
    }
}

int pin_out_val(const char *pinName, int val)
{
    if (!pinName)
        return -1;

    std::lock_guard<std::mutex> lock(g_mutex);

    int group = 0, offset = 0;
    if (!parse_pin_name(pinName, &group, &offset)) {
        if (g_bad_names.insert(pinName).second)
            printf("【GPIO】引脚名 \"%s\" 非法, 应为 \"GPIOx_Yz\" 格式(如 GPIO6_A0)\n", pinName);
        return -1;
    }

    int idx = find_pin(pinName);
    if (idx < 0) {
        /* 懒加载: 首次调用自动按输出方向打开 */
        GpioPin pin;
        pin.name = pinName;
        pin.group = group;
        pin.offset = offset;
        pin.direction = DIR_OUTPUT;
        pin.defaultVal = val;
        pin.autoOpened = true;
        int savedValue = 0;
        if (load_persistent_value(pin.name.c_str(), &savedValue))
            pin.persistedVal = savedValue;
        g_pins.push_back(pin);
        idx = (int)g_pins.size() - 1;
        printf("【GPIO】%s 未预注册, 自动按输出方向打开\n", pinName);
    }

    GpioPin *pin = &g_pins[idx];

    /* 预注册为输入的引脚不允许改输出 */
    if (!pin->autoOpened && pin->direction == DIR_INPUT) {
        log_pin_error_once(pin, "【GPIO】%s 已预注册为输入方向, 不能设置输出\n", pinName);
        return -2;
    }

    int controlledValue = -1;
    int controllerResult = controller_request("SET", pinName, val, &controlledValue);
    if (controllerResult == 0) {
        if (pin->line) {
            gpiod_line_release(pin->line);
            pin->line = nullptr;
        }
        pin->daemonManaged = true;
        pin->direction = DIR_OUTPUT;
        pin->defaultVal = val ? 1 : 0;
        if (!persist_output(pin, pin->defaultVal))
            return -3;
        pin->errLogged = false;
        return 0;
    }
    if (controllerResult < 0) {
        log_pin_error_once(pin, "【GPIO】实时控制服务无法设置 %s: %s\n",
                           pinName, strerror(errno));
        return -2;
    }
    pin->daemonManaged = false;

    /* 懒加载为输入的引脚, 改输出时自动重新打开 */
    if (pin->line && pin->autoOpened && pin->direction == DIR_INPUT) {
        gpiod_line_release(pin->line);
        pin->line = nullptr;
        pin->direction = DIR_OUTPUT;
    }

    /* 句柄为空(打开失败或尚未打开): 每次调用重试, 成功后自愈 */
    if (!pin->line) {
        pin->line = request_line(pin, true, val);
        pin->direction = DIR_OUTPUT;
        if (!pin->line)
            return -2;
        pin->errLogged = false;
    }

    if (gpiod_line_set_value(pin->line, val ? 1 : 0) < 0) {
        log_pin_error_once(pin, "【GPIO】设置 %s 输出失败\n", pinName);
        return -2;
    }
    pin->defaultVal = val ? 1 : 0;
    if (!persist_output(pin, pin->defaultVal))
        return -3;
    pin->errLogged = false;
    return 0;
}

int pin_set_high(const char *pinName)
{
    return pin_out_val(pinName, 1);
}

int pin_set_low(const char *pinName)
{
    return pin_out_val(pinName, 0);
}

int read_pin_val(const char *pinName)
{
    if (!pinName)
        return -1;

    std::lock_guard<std::mutex> lock(g_mutex);

    int group = 0, offset = 0;
    if (!parse_pin_name(pinName, &group, &offset)) {
        if (g_bad_names.insert(pinName).second)
            printf("【GPIO】引脚名 \"%s\" 非法, 应为 \"GPIOx_Yz\" 格式(如 GPIO6_A0)\n", pinName);
        return -1;
    }

    int controlledValue = -1;
    int controllerResult = controller_request("GET", pinName, 0, &controlledValue);
    if (controllerResult == 0)
        return controlledValue;
    if (controllerResult < 0) {
        printf("【GPIO】实时控制服务无法读取 %s: %s\n", pinName, strerror(errno));
        return -1;
    }

    int idx = find_pin(pinName);
    if (idx < 0) {
        /* 懒加载: 首次调用自动按输入方向打开 */
        GpioPin pin;
        pin.name = pinName;
        pin.group = group;
        pin.offset = offset;
        pin.direction = DIR_INPUT;
        pin.autoOpened = true;
        g_pins.push_back(pin);
        idx = (int)g_pins.size() - 1;
        printf("【GPIO】%s 未预注册, 自动按输入方向打开\n", pinName);
    }

    GpioPin *pin = &g_pins[idx];

    /* 句柄为空(打开失败或尚未打开): 每次调用重试 */
    if (!pin->line) {
        pin->line = request_line(pin, pin->direction == DIR_OUTPUT, pin->defaultVal);
        if (!pin->line)
            return -1;
        pin->errLogged = false;
    }

    int val = gpiod_line_get_value(pin->line);
    if (val < 0) {
        log_pin_error_once(pin, "【GPIO】读取 %s 电平失败\n", pinName);
        return -1;
    }
    pin->errLogged = false;
    return val;
}
