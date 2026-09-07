#define _POSIX_C_SOURCE 200809L

#include "common/startup_intro.h"

#include "common/anonymous_intro.h"
#include "common/cube_intro.h"
#include "common/intro_animation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t mix_seed(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

void startup_intro_play(void)
{
    const char *forced_intro = getenv("FIRST_NET_CONFIG_INTRO");
    struct timespec realtime = {0};
    struct timespec monotonic = {0};
    uint64_t seed;

    /* This override makes either presentation directly testable. */
    if (forced_intro && strcmp(forced_intro, "ghostface") == 0)
    {
        intro_animation_play();
        return;
    }
    if (forced_intro && strcmp(forced_intro, "anonymous") == 0)
    {
        anonymous_intro_play();
        return;
    }
    if (forced_intro && strcmp(forced_intro, "cube") == 0)
    {
        cube_intro_play();
        return;
    }

    (void)clock_gettime(CLOCK_REALTIME, &realtime);
    (void)clock_gettime(CLOCK_MONOTONIC, &monotonic);
    seed = (uint64_t)realtime.tv_sec ^
           ((uint64_t)realtime.tv_nsec << 21) ^
           ((uint64_t)monotonic.tv_nsec << 7) ^
           ((uint64_t)getpid() << 32);

    switch (mix_seed(seed) % UINT64_C(3))
    {
    case 0:
        intro_animation_play();
        break;
    case 1:
        anonymous_intro_play();
        break;
    default:
        cube_intro_play();
        break;
    }
}
