#pragma once

#include <stdio.h>
#include <stdarg.h>

static inline void log_printf_threadsafe(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    flockfile(stdout);
    vprintf(fmt, args);
    fflush(stdout);
    funlockfile(stdout);
    va_end(args);
}

/* 调试打印宏: 受 JSON global.debug_display 控制
 * 用法: DBG_PRINT("ch%d val=%d\n", chnId, val);
 * 注意: 使用本宏的 .cpp 文件需已包含 app_ctrl.h (大多数文件已包含) */
#ifdef __cplusplus
#define DBG_PRINT(fmt, ...)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if (app_ctrl_get_debug_display())                                                                              \
            log_printf_threadsafe(fmt, ##__VA_ARGS__);                                                                 \
    } while (0)
#else
#define DBG_PRINT(fmt, ...) ((void)0)

#endif
