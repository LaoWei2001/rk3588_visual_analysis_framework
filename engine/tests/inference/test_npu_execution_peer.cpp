#include "vision/inference/npu_execution.h"

// A separate translation unit represents a second model implementation.
int peer_infer(int core_mask)
{
    npu::Execution execution(core_mask);
    rknn_output output{};
    rknn_input input{};
    int rc = execution.inputs_set(2, 1, &input);
    if (rc < 0)
        return rc;
    rc = execution.run(2);
    if (rc < 0)
        return rc;
    npu::Outputs outputs(execution, 2, 1, &output);
    rc = outputs.get();
    if (rc < 0)
        return rc;
    return outputs.release();
}
