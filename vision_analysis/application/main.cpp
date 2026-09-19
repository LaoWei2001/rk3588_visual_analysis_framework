#include <rkvision/application.h>

// Generated from this project's manifests; never compiled into librkvision.
extern const char *logic_embedded_catalog_json();

int main(int argc, char **argv)
{
    const RkVisionApplication application = {
        RKVISION_RUNTIME_ABI, sizeof(RkVisionApplication),
        RKVISION_BUILD_ABI_TAG, logic_embedded_catalog_json()
    };
    return rkvision_run(argc, argv, &application);
}
