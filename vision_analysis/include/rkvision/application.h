#ifndef RKVISION_APPLICATION_H
#define RKVISION_APPLICATION_H

#include <stdint.h>

#define RKVISION_RUNTIME_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

/* One application per process. Strings must remain valid until rkvision_run returns.
 * The C entry point does not make the C++ logic SDK ABI independent of its compiler.
 * The project CMake target supplies the SDK/compiler compatibility tag. */
typedef struct RkVisionApplication {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *abi_tag;
    const char *catalog_json;
} RkVisionApplication;

int rkvision_run(int argc, char **argv, const RkVisionApplication *application);
const char *rkvision_runtime_version(void);

#ifdef __cplusplus
}
#endif
#endif
