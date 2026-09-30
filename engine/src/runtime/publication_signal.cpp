#include "publication_signal.h"

#include <cerrno>
#include <pthread.h>
#include <time.h>

namespace
{
pthread_mutex_t g_publication_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_publication_cv = PTHREAD_COND_INITIALIZER;
uint64_t g_publication_sequence = 0;
}

uint64_t publication_signal_sequence(void)
{
    pthread_mutex_lock(&g_publication_mtx);
    const uint64_t value = g_publication_sequence;
    pthread_mutex_unlock(&g_publication_mtx);
    return value;
}

void publication_signal_notify(void)
{
    pthread_mutex_lock(&g_publication_mtx);
    ++g_publication_sequence;
    pthread_cond_broadcast(&g_publication_cv);
    pthread_mutex_unlock(&g_publication_mtx);
}

bool publication_signal_wait(uint64_t observed_sequence, int timeout_ms)
{
    if (timeout_ms <= 0)
        return publication_signal_sequence() != observed_sequence;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_publication_mtx);
    int rc = 0;
    while (g_publication_sequence == observed_sequence && rc == 0)
        rc = pthread_cond_timedwait(&g_publication_cv, &g_publication_mtx, &deadline);
    const bool changed = g_publication_sequence != observed_sequence;
    pthread_mutex_unlock(&g_publication_mtx);
    return changed;
}
