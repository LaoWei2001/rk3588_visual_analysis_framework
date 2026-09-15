#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_RELAY_PIN "GPIO6_A2"
#define INSTALLED_GPIOCTL "/usr/local/bin/rk3588-gpioctl"

static void usage(const char *program)
{
    fprintf(stderr,
            "用法：\n"
            "  %s                         切换默认继电器 GPIO6_A2\n"
            "  %s set <0|1|low|high>      明确设置默认继电器\n"
            "  %s get                     读取默认继电器\n"
            "  %s --pin GPIOx_Yz set <值> 测试其他板卡上的继电器引脚\n",
            program, program, program, program);
}

int main(int argc, char **argv)
{
    const char *override = getenv("RK3588_GPIOCTL_BIN");
    if (!override || !override[0])
        override = getenv("GPIO_TEST_BIN"); /* 兼容旧开发环境变量。 */
    const char *gpioctl = override && override[0]
                              ? override : INSTALLED_GPIOCTL;
    int has_explicit_pin = argc > 2 && strcmp(argv[1], "--pin") == 0;
    int extra_arguments = has_explicit_pin ? 0 : 2;
    /* argv[0] + 可选的 --pin/PIN + 原参数或默认 toggle + 结尾 NULL。 */
    size_t forwarded_capacity = (size_t)argc + (size_t)extra_arguments + 2U;
    char **forwarded;
    int output = 0;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--help") == 0))
    {
        usage(argv[0]);
        return 0;
    }

    forwarded = calloc(forwarded_capacity, sizeof(*forwarded));
    if (!forwarded)
    {
        fprintf(stderr, "无法准备继电器测试参数：%s\n", strerror(errno));
        return 1;
    }
    forwarded[output++] = (char *)gpioctl;
    if (!has_explicit_pin)
    {
        forwarded[output++] = "--pin";
        forwarded[output++] = DEFAULT_RELAY_PIN;
    }
    if (argc == 1)
    {
        forwarded[output++] = "toggle";
    }
    else
    {
        for (int index = 1; index < argc; index++)
            forwarded[output++] = argv[index];
    }
    forwarded[output] = NULL;

    execv(gpioctl, forwarded);
    if (!override || !override[0])
    {
        /* 允许开发环境仅把控制工具放在 PATH 中。 */
        forwarded[0] = "rk3588-gpioctl";
        execvp(forwarded[0], forwarded);
        forwarded[0] = "gpio_test";
        execvp(forwarded[0], forwarded);
    }
    fprintf(stderr,
            "无法启动 GPIO 控制工具 %s：%s\n"
            "请从项目根目录运行统一安装器：\n"
            "  sudo ./install.sh online\n",
            gpioctl, strerror(errno));
    free(forwarded);
    return 1;
}
