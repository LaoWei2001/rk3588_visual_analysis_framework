// Test-only LD_PRELOAD shim for CLI runs (without the systemd watchdog).
// The runner verifies that the failure actually reached the composite executor.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
static atomic_int failed = 0;
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    int (*real_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) =
        dlsym(RTLD_NEXT, "pthread_create");
    Dl_info info = {0};
    if (dladdr(__builtin_return_address(0), &info) && info.dli_fname && strstr(info.dli_fname, "libstdc++") &&
        !atomic_exchange(&failed, 1))
        return EAGAIN;
    return real_create(thread, attr, start, arg);
}
