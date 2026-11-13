#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /** Per-channel preview worker; main owns thread creation and joining. */
    void *display_worker_thread(void *arg);

#ifdef __cplusplus
}
#endif
