#pragma once

#include "inference/npu_execution.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace model_io
{
// Instance-owned outputs. For verified quantized/FP16/FP32 layouts, the synchronous
// RKNN run writes directly to native DMA buffers. Repacking happens after the
// core lease ends. Other layouts retain the checked get/release API.
class OutputBuffers
{
  public:
    ~OutputBuffers() { reset(); }
    OutputBuffers() = default;
    OutputBuffers(const OutputBuffers &) = delete;
    OutputBuffers &operator=(const OutputBuffers &) = delete;

    void reset()
    {
        for (auto *memory : memories_) rknn_destroy_mem(context_, memory);
        memories_.clear();
        native_ = false;
    }

    void initialize(rknn_context context, const std::vector<rknn_tensor_attr> &logical, bool want_float,
                    bool allow_native = true)
    {
        reset();
        context_ = context;
        logical_ = logical;
        want_float_ = want_float;
        buffers_.resize(logical.size());
        outputs_.resize(logical.size());
        native_attrs_.resize(logical.size());
        native_ = allow_native && !logical.empty();
        for (size_t i = 0; i < logical.size(); ++i)
        {
            const auto &a = logical[i];
            const size_t bytes = size_t(a.n_elems) * (want_float ? sizeof(float) : 1);
            if (!bytes || bytes > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("Invalid model output size");
            buffers_[i].resize((bytes + sizeof(float) - 1) / sizeof(float));
            auto &n = native_attrs_[i];
            n = {}; n.index = i;
            if (rknn_query(context, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &n, sizeof(n)) < 0 ||
                !supported(a, n, want_float)) native_ = false;
        }
        if (native_)
        {
            try
            {
                // Allocate everything before binding, so a failed allocation
                // cannot leave a partially bound model in use.
                for (const auto &n : native_attrs_)
                {
                    auto *memory = rknn_create_mem(context, std::max(n.size, n.size_with_stride));
                    if (memory) memories_.push_back(memory);
                    if (!memory || !memory->virt_addr || memory->size < std::max(n.size, n.size_with_stride))
                        throw std::runtime_error("Cannot allocate native model output");
                }
                for (size_t i = 0; i < memories_.size(); ++i)
                {
                    auto n = native_attrs_[i];
                    if (rknn_set_io_mem(context, memories_[i], &n) < 0)
                        throw std::runtime_error("Cannot bind native model output");
                }
            }
            catch (...) { reset(); throw; }
        }
        prepare();
    }

    void prepare()
    {
        for (size_t i = 0; i < outputs_.size(); ++i)
        {
            auto &o = outputs_[i]; o = {};
            o.index = i; o.want_float = want_float_; o.is_prealloc = 1;
            o.buf = buffers_[i].data();
            o.size = logical_[i].n_elems * (want_float_ ? sizeof(float) : 1);
        }
    }

    int collect(npu::Execution &execution)
    {
        if (native_) return 0; // non_block=0, no ASYNC flag: outputs already complete
        npu::Outputs lease(execution, context_, outputs_.size(), outputs_.data());
        const int result = lease.get();
        return result < 0 ? result : lease.release();
    }

    void decode()
    {
        if (!native_) return;
        for (size_t i = 0; i < outputs_.size(); ++i)
        {
            if (want_float_) unpack_float(logical_[i], native_attrs_[i], memories_[i]->virt_addr,
                                          static_cast<float *>(outputs_[i].buf));
            else unpack(logical_[i], native_attrs_[i], memories_[i]->virt_addr, outputs_[i].buf);
        }
    }
    rknn_output *data() { return outputs_.data(); }
    bool native() const { return native_; }

    int execute(int mask, rknn_input *inputs, uint32_t input_count,
                std::chrono::steady_clock::time_point &ran)
    {
        prepare();
        {
            npu::Execution execution(mask);
            int result = inputs ? execution.inputs_set(context_, input_count, inputs) : 0;
            if (result < 0) return result;
            result = execution.run(context_);
            if (result < 0) return result;
            ran = std::chrono::steady_clock::now();
            result = collect(execution);
            if (result < 0) return result;
        }
        decode();
        return 0;
    }

    static bool supported(const rknn_tensor_attr &a, const rknn_tensor_attr &n, bool want_float)
    {
        const bool quantized = a.type == RKNN_TENSOR_INT8 || a.type == RKNN_TENSOR_UINT8;
        const unsigned element_bytes = a.type == RKNN_TENSOR_FLOAT16 ? 2 : a.type == RKNN_TENSOR_FLOAT32 ? 4 : 1;
        if (want_float && quantized && (a.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
                                       !std::isfinite(a.scale) || a.scale <= 0)) return false;
        // Exact float readback rounding is verified for the pinned SDK's
        // NC1HWC2 INT8/UINT8 C2=16 kernel; keep other quantized layouts on SDK get.
        if (want_float && quantized && (n.fmt != RKNN_TENSOR_NC1HWC2 || n.n_dims != 5 || n.dims[4] != 16))
            return false;
        if ((!quantized && !(want_float && (a.type == RKNN_TENSOR_FLOAT16 || a.type == RKNN_TENSOR_FLOAT32))) ||
            a.type != n.type || a.zp != n.zp || a.scale != n.scale || a.qnt_type != n.qnt_type) return false;
        // A flat UNDEFINED tensor has no channel packing. RKNN may reserve
        // a larger backing allocation (the OBB angle plane is one example).
        if (a.fmt == RKNN_TENSOR_UNDEFINED && n.fmt == RKNN_TENSOR_UNDEFINED &&
            a.n_dims >= 1 && a.n_dims <= 4 && a.n_dims == n.n_dims && a.n_elems &&
            n.n_elems == a.n_elems && std::equal(a.dims, a.dims+a.n_dims, n.dims) &&
            !n.w_stride && !n.h_stride)
        {
            uint64_t elements = 1;
            for (unsigned i = 0; i < a.n_dims; ++i) elements *= a.dims[i];
            return elements == a.n_elems && uint64_t(a.n_elems) * element_bytes <= std::max(n.size, n.size_with_stride);
        }
        if (a.n_dims != 4 || a.fmt != RKNN_TENSOR_NCHW || a.dims[0] != 1 ||
            !a.dims[1] || !a.dims[2] || !a.dims[3] ||
            uint64_t(a.dims[1]) * a.dims[2] * a.dims[3] != a.n_elems) return false;
        if (n.fmt == RKNN_TENSOR_NC1HWC2 && n.n_dims == 5 && n.dims[0] == 1 && n.dims[4] &&
            uint64_t(n.dims[1]) * n.dims[4] >= a.dims[1] &&
            n.dims[2] == a.dims[2] && n.dims[3] == a.dims[3])
        {
            const uint64_t bytes = uint64_t(n.dims[1]) * n.dims[2] *
                std::max(n.dims[3], n.w_stride) * n.dims[4] * element_bytes;
            // Reject extra height padding and unknown layouts instead of guessing.
            return (!n.h_stride || n.h_stride == n.dims[2]) && bytes &&
                bytes <= std::max(n.size, n.size_with_stride);
        }
        return n.fmt == RKNN_TENSOR_NCHW && n.n_dims == a.n_dims &&
            std::equal(a.dims, a.dims + a.n_dims, n.dims) &&
            (!n.w_stride || n.w_stride == a.dims[3]) && (!n.h_stride || n.h_stride == a.dims[2]) &&
            std::max(n.size, n.size_with_stride) >= uint64_t(a.n_elems) * element_bytes;
    }

    static void unpack_float(const rknn_tensor_attr &a, const rknn_tensor_attr &n, const void *source, float *target)
    {
        switch (n.type)
        {
        case RKNN_TENSOR_INT8: unpack_float_as(a, n, static_cast<const int8_t *>(source), target); break;
        case RKNN_TENSOR_UINT8: unpack_float_as(a, n, static_cast<const uint8_t *>(source), target); break;
        case RKNN_TENSOR_FLOAT16: unpack_float_as(a, n, static_cast<const uint16_t *>(source), target); break;
        case RKNN_TENSOR_FLOAT32: unpack_float_as(a, n, static_cast<const float *>(source), target); break;
        default: throw std::logic_error("Unsupported native output dtype");
        }
    }

    static void unpack(const rknn_tensor_attr &a, const rknn_tensor_attr &n, const void *source, void *target)
    {
        if (n.fmt != RKNN_TENSOR_NC1HWC2) { std::memcpy(target, source, a.n_elems); return; }
        const auto *src = static_cast<const uint8_t *>(source);
        auto *dst = static_cast<uint8_t *>(target);
        const size_t channels = a.dims[1], height = a.dims[2], width = a.dims[3];
        const size_t c2 = n.dims[4], stride = std::max(n.w_stride, n.dims[3]);
#ifdef __aarch64__
        if (c2 == 16)
        {
            for (size_t c = 0; c < channels; c += 16)
                for (size_t y = 0; y < height; ++y)
                {
                    const auto *row = src + ((c / 16 * height + y) * stride) * 16;
                    size_t x = 0;
                    for (; x + 16 <= width; x += 16)
                    {
                        // Transpose 16 pixels x 16 channels without arithmetic:
                        // preserve quantized values exactly, including padding.
                        uint8x16_t v[16], t[16];
                        for (int k = 0; k < 16; ++k) v[k] = vld1q_u8(row + (x + k) * 16);
                        for (int k = 0; k < 16; k += 2) {
                            t[k] = vtrn1q_u8(v[k], v[k+1]);
                            t[k+1] = vtrn2q_u8(v[k], v[k+1]);
                        }
                        for (int b = 0; b < 16; b += 4) for (int k = b; k < b+2; ++k) {
                            v[k] = vreinterpretq_u8_u16(vtrn1q_u16(vreinterpretq_u16_u8(t[k]), vreinterpretq_u16_u8(t[k+2])));
                            v[k+2] = vreinterpretq_u8_u16(vtrn2q_u16(vreinterpretq_u16_u8(t[k]), vreinterpretq_u16_u8(t[k+2])));
                        }
                        for (int b = 0; b < 16; b += 8) for (int k = b; k < b+4; ++k) {
                            t[k] = vreinterpretq_u8_u32(vtrn1q_u32(vreinterpretq_u32_u8(v[k]), vreinterpretq_u32_u8(v[k+4])));
                            t[k+4] = vreinterpretq_u8_u32(vtrn2q_u32(vreinterpretq_u32_u8(v[k]), vreinterpretq_u32_u8(v[k+4])));
                        }
                        for (int k = 0; k < 8; ++k) {
                            v[k] = vreinterpretq_u8_u64(vtrn1q_u64(vreinterpretq_u64_u8(t[k]), vreinterpretq_u64_u8(t[k+8])));
                            v[k+8] = vreinterpretq_u8_u64(vtrn2q_u64(vreinterpretq_u64_u8(t[k]), vreinterpretq_u64_u8(t[k+8])));
                        }
                        for (size_t k = 0; k < 16 && c+k < channels; ++k)
                            vst1q_u8(dst + ((c+k) * height + y) * width + x, v[k]);
                    }
                    for (size_t k = 0; k < 16 && c+k < channels; ++k)
                        for (size_t tail = x; tail < width; ++tail)
                            dst[((c+k) * height + y) * width + tail] = row[tail * 16 + k];
                }
            return;
        }
#endif
        for (size_t c = 0; c < channels; ++c)
            for (size_t y = 0; y < height; ++y)
            {
                const auto *row = src + ((c / c2 * height + y) * stride) * c2 + c % c2;
                auto *out = dst + (c * height + y) * width;
                for (size_t x = 0; x < width; ++x) out[x] = row[x * c2];
            }
    }

  private:
    // Match RKNN's float readback rounding: scale*q plus a rounded offset.
    static float value(int8_t v, float scale, float offset) { return std::fma(float(v), scale, offset); }
    static float value(uint8_t v, float scale, float offset) { return std::fma(float(v), scale, offset); }
    static float value(float v, float, float) { return v; }
    static float value(uint16_t v, float, float)
    {
#ifdef __aarch64__
        __fp16 half;
        std::memcpy(&half, &v, sizeof(v));
        return float(half);
#else
        const uint32_t sign = uint32_t(v & 0x8000) << 16;
        uint32_t mantissa = v & 0x3ff, exponent = (v >> 10) & 0x1f, bits;
        if (!exponent && mantissa) {
            int shift = 0;
            while (!(mantissa & 0x400)) { mantissa <<= 1; ++shift; }
            bits = sign | (uint32_t(113 - shift) << 23) | ((mantissa & 0x3ff) << 13);
        } else if (!exponent) bits = sign;
        else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
        else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float result; std::memcpy(&result, &bits, sizeof(result)); return result;
#endif
    }
    static float tail_value(int8_t v, float scale, float zp) { return (float(v)-zp)*scale; }
    static float tail_value(uint8_t v, float scale, float zp) { return (float(v)-zp)*scale; }
    static float tail_value(float v, float, float) { return v; }
    static float tail_value(uint16_t v, float, float) { return value(v, 0.f, 0.f); }
    template <typename T>
    static void unpack_float_as(const rknn_tensor_attr &a, const rknn_tensor_attr &n, const T *source, float *target)
    {
        const float scale = n.scale, zp = float(n.zp), offset = -zp * scale;
        if (n.fmt != RKNN_TENSOR_NC1HWC2) {
            for (size_t k = 0; k < a.n_elems; ++k) target[k] = value(source[k], scale, offset);
            return;
        }
        const size_t channels = a.dims[1], height = a.dims[2], width = a.dims[3];
        const size_t c2 = n.dims[4], stride = std::max(n.w_stride, n.dims[3]);
        for (size_t c = 0; c < channels; ++c)
            for (size_t y = 0; y < height; ++y)
            {
                const auto *row = source + ((c / c2 * height + y) * stride) * c2 + c % c2;
                auto *out = target + (c * height + y) * width;
                // Pinned RKNN uses FMA for complete channel blocks and the
                // scalar (q-zp)*scale path for the final partial block.
                if (c >= channels/c2*c2)
                    for (size_t x = 0; x < width; ++x) out[x] = tail_value(row[x*c2], scale, zp);
                else
                    for (size_t x = 0; x < width; ++x) out[x] = value(row[x*c2], scale, offset);
            }
    }
    rknn_context context_ = 0;
    bool native_ = false, want_float_ = false;
    std::vector<rknn_tensor_attr> logical_, native_attrs_;
    std::vector<rknn_tensor_mem *> memories_;
    // float alignment also serves the get(want_float=1) fallback.
    std::vector<std::vector<float>> buffers_;
    std::vector<rknn_output> outputs_;
};
} // namespace model_io
