#pragma once

#include "npu_core_gate.h"
#include <rknn_api.h>

namespace npu
{
// Keep this object alive through get/release for SDK-owned outputs. With bound
// native outputs, a blocking run completes all device writes before unlocking.
// CPU-input and DMA-input adapters share the same gate.
class Execution
{
  public:
    explicit Execution(int core_mask) : lease_(core_mask)
    {
    }

    int run(rknn_context context)
    {
        lease_.check_health();
        rknn_run_extend options{};
        options.timeout_ms = 5000;
        options.fence_fd = -1;
        return checked(rknn_run(context, &options));
    }

    int inputs_set(rknn_context context, uint32_t count, rknn_input *inputs)
    {
        lease_.check_health();
        return checked(rknn_inputs_set(context, count, inputs));
    }

    int outputs_get(rknn_context context, uint32_t count, rknn_output *outputs)
    {
        return checked(rknn_outputs_get(context, count, outputs, nullptr));
    }

    int outputs_release(rknn_context context, uint32_t count, rknn_output *outputs)
    {
        return checked(rknn_outputs_release(context, count, outputs));
    }

  private:
    int checked(int result)
    {
        if (result < 0)
            lease_.mark_failed();
        return result;
    }
    CoreLease lease_;
};

// Declare after the output descriptors, with Execution alive until release.
// Vendor buffers are released even when the model's postprocessing throws.
class Outputs
{
  public:
    Outputs(Execution &execution, rknn_context context, uint32_t count, rknn_output *outputs)
        : execution_(execution), context_(context), count_(count), outputs_(outputs)
    {
    }
    ~Outputs()
    {
        if (acquired_)
        {
            try
            {
                release();
            }
            catch (...)
            {
            }
        }
    }
    Outputs(const Outputs &) = delete;
    Outputs &operator=(const Outputs &) = delete;
    int get()
    {
        const int rc = execution_.outputs_get(context_, count_, outputs_);
        acquired_ = rc >= 0;
        return rc;
    }
    int release()
    {
        if (!acquired_)
            return 0;
        acquired_ = false;
        return execution_.outputs_release(context_, count_, outputs_);
    }

  private:
    Execution &execution_;
    rknn_context context_;
    uint32_t count_;
    rknn_output *outputs_;
    bool acquired_ = false;
};
} // namespace npu
