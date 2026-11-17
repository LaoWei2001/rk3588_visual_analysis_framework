#include "image_inference_cli.h"
#include "model_inspection_cli.h"
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--image-inference-capabilities") == 0)
        return image_inference_capabilities_cli();
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--inspect-model") == 0)
        return inspect_model_cli(argv[2], argc == 4 ? argv[3] : "");
    if (argc == 3 && std::strcmp(argv[1], "--infer-images") == 0)
        return image_inference_cli(argv[2]);
    std::fprintf(
        stderr,
        "Usage: %s --image-inference-capabilities | --inspect-model model.rknn [type] | --infer-images job.json\n",
        argv[0]);
    return 2;
}
