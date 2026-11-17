#include <model_inspection.h>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " model.rknn model_type\n";
        return 2;
    }
    // One call when you only need a single decoder check.
    const auto checked = vision::check_model_file(argv[1], argv[2]);
    if (!checked.inspection.ok)
    {
        std::cerr << checked.error() << '\n';
        return 1;
    }
    const auto &metadata = checked.inspection.metadata;
    std::cout << "inputs=" << metadata.inputs.size() << " outputs=" << metadata.outputs.size() << '\n';
    for (const auto &tensor : metadata.outputs)
    {
        std::cout << tensor.name << " " << vision::tensor_layout_name(tensor.layout) << " [";
        for (auto dim : tensor.shape) std::cout << dim << " ";
        std::cout << "] elements=" << tensor.elements << " scale=" << tensor.scale << '\n';
    }
    // Reusing metadata never reloads the model or accesses the NPU.
    for (const auto &info : vision::inference_model_types())
    {
        const auto status = vision::check_model_compatibility(metadata, info.type);
        std::cout << info.type << ": " << (status.ok ? "compatible" : status.error) << '\n';
    }
    if (!checked.ok())
    {
        std::cerr << checked.error() << '\n';
        return 1;
    }
    std::cout << "selected: " << checked.compatibility.input_width << " x "
              << checked.compatibility.input_height << " "
              << vision::tensor_layout_name(checked.compatibility.input_layout) << '\n';
    return 0;
}
