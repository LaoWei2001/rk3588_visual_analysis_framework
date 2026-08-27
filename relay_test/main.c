#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GPIO_CHIP_NAME "gpiochip6"
#define GPIO_LINE_OFFSET 2U /* GPIO6_A2，沿用 08_GPIO/test-relay */
#define GPIO_CONSUMER "relay_test"
#define STATE_FILE "/var/lib/relay_test.state"

static int read_saved_value(int fd, int *value)
{
    char buffer[8] = {0};
    ssize_t count = pread(fd, buffer, sizeof(buffer) - 1, 0);

    if (count < 1)
    {
        return -1;
    }
    if ((buffer[0] == '0' || buffer[0] == '1') &&
        (count == 1 || buffer[1] == '\n' || buffer[1] == '\0'))
    {
        *value = buffer[0] - '0';
        return 0;
    }
    return -1;
}

static int read_physical_value(struct gpiod_line *line, int *value)
{
    int result;

    if (gpiod_line_request_input(line, GPIO_CONSUMER) < 0)
    {
        return -1;
    }
    result = gpiod_line_get_value(line);
    gpiod_line_release(line);
    if (result != 0 && result != 1)
    {
        return -1;
    }
    *value = result;
    return 0;
}

static int save_value(int fd, int value)
{
    char text[2] = {(char)('0' + value), '\n'};

    if (ftruncate(fd, 0) != 0 || pwrite(fd, text, sizeof(text), 0) != 2 ||
        fsync(fd) != 0)
    {
        return -1;
    }
    return 0;
}

int main(void)
{
    struct gpiod_chip *chip = NULL;
    struct gpiod_line *line = NULL;
    struct stat state_info;
    int state_fd = -1;
    int current_value;
    int next_value;
    int readback;
    int exit_code = 1;

    state_fd = open(STATE_FILE,
                    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                    0600);
    if (state_fd < 0)
    {
        fprintf(stderr, "无法打开状态文件 %s：%s（请使用 sudo 运行）\n",
                STATE_FILE, strerror(errno));
        goto cleanup;
    }
    if (flock(state_fd, LOCK_EX) != 0 || fstat(state_fd, &state_info) != 0 ||
        !S_ISREG(state_info.st_mode) || state_info.st_uid != geteuid() ||
        fchmod(state_fd, 0600) != 0)
    {
        fprintf(stderr, "状态文件不安全或无法锁定：%s\n", STATE_FILE);
        goto cleanup;
    }

    chip = gpiod_chip_open_by_name(GPIO_CHIP_NAME);
    if (!chip)
    {
        fprintf(stderr, "无法打开 %s：%s\n", GPIO_CHIP_NAME, strerror(errno));
        goto cleanup;
    }
    line = gpiod_chip_get_line(chip, GPIO_LINE_OFFSET);
    if (!line)
    {
        fprintf(stderr, "无法获取 GPIO6_A2：%s\n", strerror(errno));
        goto cleanup;
    }

    if (read_saved_value(state_fd, &current_value) != 0 &&
        read_physical_value(line, &current_value) != 0)
    {
        fprintf(stderr, "无法读取 GPIO6_A2 当前状态：%s\n", strerror(errno));
        goto cleanup;
    }
    next_value = 1 - current_value;

    if (gpiod_line_request_output(line, GPIO_CONSUMER, next_value) < 0)
    {
        fprintf(stderr, "无法控制 GPIO6_A2：%s\n", strerror(errno));
        goto cleanup;
    }
    readback = gpiod_line_get_value(line);
    if (readback != next_value)
    {
        fprintf(stderr, "GPIO6_A2 输出校验失败，期望 %d，读取到 %d\n",
                next_value, readback);
        goto cleanup;
    }
    if (save_value(state_fd, next_value) != 0)
    {
        fprintf(stderr, "继电器已切换，但无法保存状态：%s\n", strerror(errno));
        goto cleanup;
    }

    printf("继电器 GPIO6_A2：%d -> %d\n", current_value, next_value);
    exit_code = 0;

cleanup:
    if (line && gpiod_line_is_requested(line))
    {
        gpiod_line_release(line);
    }
    if (chip)
    {
        gpiod_chip_close(chip);
    }
    if (state_fd >= 0)
    {
        close(state_fd);
    }
    return exit_code;
}
