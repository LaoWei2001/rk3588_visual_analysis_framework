#include <rkvision/application.h>
#include "application_internal.h"

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" const char *rkvision_runtime_version(void)
{
    return RKVISION_ENGINE_VERSION;
}

extern "C" int rkvision_run(int argc, char **argv, const RkVisionApplication *application)
{
    if (!application || application->abi_version != RKVISION_RUNTIME_ABI ||
        application->struct_size != sizeof(RkVisionApplication) ||
        !application->abi_tag || std::strcmp(application->abi_tag, RKVISION_BUILD_ABI_TAG) != 0 ||
        !application->catalog_json) {
        std::fprintf(stderr, "RKVision: incompatible application/runtime ABI; rebuild the project with this engine SDK.\n");
        return 2;
    }
    static std::atomic_flag started = ATOMIC_FLAG_INIT;
    if (started.test_and_set()) {
        std::fprintf(stderr, "RKVision: only one application run per process is supported.\n");
        return 2;
    }
    rkvision_set_application_catalog(application->catalog_json);
    return rkvision_engine_main(argc, argv);
}
