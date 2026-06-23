#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>

/* 注意: <rga/RgaApi.h> 和 <gtk/gtk.h> 已从此处移除。
 * 需要 RGA 的文件请直接 #include <rga/RgaApi.h>
 * 需要 GTK 的文件请直接 #include <gtk/gtk.h>
 */

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
        extern struct APP_CTRL *g_pCtrl;                                                                               \
        if (g_pCtrl && g_pCtrl->config.debug_display)                                                                  \
            log_printf_threadsafe(fmt, ##__VA_ARGS__);                                                                 \
    } while (0)
#else
#define DBG_PRINT(fmt, ...) ((void)0)
#endif

#endif
