#include "vision/models/rknn_output_buffers.h"
#include <cassert>
#include <random>

int main()
{
    std::mt19937 random(3588);
    for (unsigned channels : {1, 3, 15, 16, 17, 65, 80, 120})
        for (unsigned width : {1, 7, 19})
            for (unsigned c2 : {8, 16})
                for (unsigned padding : {0, 3})
                {
                    rknn_tensor_attr a{}, n{};
                    a.n_dims = 4;
                    a.fmt = RKNN_TENSOR_NCHW;
                    a.type = RKNN_TENSOR_INT8;
                    a.qnt_type = RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
                    a.scale = .005f;
                    a.dims[0] = 1;
                    a.dims[1] = channels;
                    a.dims[2] = 3;
                    a.dims[3] = width;
                    a.n_elems = channels * 3 * width;
                    a.size = a.n_elems;
                    n = a;
                    n.n_dims = 5;
                    n.fmt = RKNN_TENSOR_NC1HWC2;
                    // RKNN can pad beyond the minimum C1 (e.g. 80 -> 96 channels).
                    n.dims[1] = (channels + c2 - 1) / c2 + 1;
                    n.dims[4] = c2;
                    n.w_stride = width + padding;
                    n.size = n.size_with_stride = n.dims[1] * 3 * n.w_stride * c2;
                    std::vector<uint8_t> source(n.size, 0xEF), expected(a.n_elems), actual(a.n_elems + 16, 0xBA);
                    for (unsigned c = 0; c < channels; ++c)
                        for (unsigned y = 0; y < 3; ++y)
                            for (unsigned x = 0; x < width; ++x)
                            {
                                const uint8_t value = random();
                                expected[(c * 3 + y) * width + x] = value;
                                source[((c / c2 * 3 + y) * n.w_stride + x) * c2 + c % c2] = value;
                            }
                    assert(model_io::OutputBuffers::supported(a, n, false));
                    model_io::OutputBuffers::unpack(a, n, source.data(), actual.data());
                    assert(std::equal(expected.begin(), expected.end(), actual.begin()));
                    for (unsigned i = a.n_elems; i < actual.size(); ++i)
                        assert(actual[i] == 0xBA);
                    assert(model_io::OutputBuffers::supported(a, n, true) == (c2 == 16));
                    a.zp = n.zp = -37;
                    a.scale = n.scale = .005f;
                    std::vector<float> floats(a.n_elems + 1, -999.f);
                    model_io::OutputBuffers::unpack_float(a, n, source.data(), floats.data());
                    for (unsigned k = 0; k < a.n_elems; ++k)
                        assert(floats[k] == (k / (3 * width) >= channels / c2 * c2
                                                 ? (float(static_cast<int8_t>(expected[k])) + 37.f) * .005f
                                                 : float(double(static_cast<int8_t>(expected[k])) * double(.005f) +
                                                         double(37.f * .005f))));
                    assert(floats.back() == -999.f);
                    a.type = n.type = RKNN_TENSOR_UINT8;
                    model_io::OutputBuffers::unpack_float(a, n, source.data(), floats.data());
                    for (unsigned k = 0; k < a.n_elems; ++k)
                        assert(floats[k] == (k / (3 * width) >= channels / c2 * c2
                                                 ? (float(expected[k]) + 37.f) * .005f
                                                 : float(double(expected[k]) * double(.005f) + double(37.f * .005f))));
                    auto bad = n;
                    bad.size = bad.size_with_stride = 1;
                    assert(!model_io::OutputBuffers::supported(a, bad, false));
                    bad = n;
                    bad.h_stride = 8;
                    assert(!model_io::OutputBuffers::supported(a, bad, false));
                    bad = n;
                    bad.dims[4] = 0;
                    assert(!model_io::OutputBuffers::supported(a, bad, false));
                    bad = n;
                    bad.scale = 1;
                    assert(!model_io::OutputBuffers::supported(a, bad, false));
                    n = a;
                    assert(model_io::OutputBuffers::supported(a, n, false));
                    model_io::OutputBuffers::unpack(a, n, expected.data(), actual.data());
                    assert(std::equal(expected.begin(), expected.end(), actual.begin()));
                }
    // FP16 includes subnormal, signed zero, infinities and NaN. Native padding
    // must never leak into the logical float plane.
    rknn_tensor_attr a{}, n{};
    a.n_dims = 4;
    a.fmt = RKNN_TENSOR_NCHW;
    a.type = RKNN_TENSOR_FLOAT16;
    a.dims[0] = 1;
    a.dims[1] = 3;
    a.dims[2] = 2;
    a.dims[3] = 5;
    a.n_elems = 30;
    a.size = 60;
    n = a;
    n.n_dims = 5;
    n.fmt = RKNN_TENSOR_NC1HWC2;
    n.dims[1] = 1;
    n.dims[4] = 8;
    n.size = n.size_with_stride = 160;
    const uint16_t halves[] = {0, 0x8000, 0x3c00, 0xc000, 0x7c00, 0xfc00, 1, 0x7e00};
    const float expected[] = {0.f, -0.f, 1.f, -2.f, INFINITY, -INFINITY, 0x1p-24f, NAN};
    std::vector<uint16_t> half_source(80, 0xdead);
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned k = 0; k < 10; ++k)
            half_source[k * 8 + c] = halves[(c * 10 + k) % 8];
    std::vector<float> floats(31, -999.f);
    assert(model_io::OutputBuffers::supported(a, n, true));
    assert(!model_io::OutputBuffers::supported(a, n, false));
    model_io::OutputBuffers::unpack_float(a, n, half_source.data(), floats.data());
    for (unsigned k = 0; k < 30; ++k)
    {
        if (k % 8 == 7)
            assert(std::isnan(floats[k]));
        else
            assert(std::memcmp(&floats[k], &expected[k % 8], sizeof(float)) == 0);
    }
    assert(floats.back() == -999.f);
    a.type = n.type = RKNN_TENSOR_FLOAT32;
    a.size *= 2;
    n.size = n.size_with_stride = 320;
    std::vector<float> float_source(80, -999.f);
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned k = 0; k < 10; ++k)
            float_source[k * 8 + c] = float(c * 10 + k) / 13.f;
    assert(model_io::OutputBuffers::supported(a, n, true));
    model_io::OutputBuffers::unpack_float(a, n, float_source.data(), floats.data());
    for (unsigned k = 0; k < 30; ++k)
        assert(floats[k] == float(k) / 13.f);
    assert(floats.back() == -999.f);
    a = {};
    a.type = RKNN_TENSOR_INT8;
    a.fmt = RKNN_TENSOR_UNDEFINED;
    a.n_dims = 3;
    a.dims[0] = 1;
    a.dims[1] = 1;
    a.dims[2] = 37;
    a.n_elems = a.size = 37;
    n = a;
    n.size = n.size_with_stride = 37 * 16;
    std::vector<uint8_t> flat(n.size, 0xEE), logical(38, 0xBA);
    for (unsigned k = 0; k < 37; ++k)
        flat[k] = k;
    assert(model_io::OutputBuffers::supported(a, n, false));
    model_io::OutputBuffers::unpack(a, n, flat.data(), logical.data());
    assert(std::equal(flat.begin(), flat.begin() + 37, logical.begin()));
    assert(logical.back() == 0xBA);
    n.w_stride = 2;
    assert(!model_io::OutputBuffers::supported(a, n, false));
    n.w_stride = 0;
    n.n_elems = 36;
    assert(!model_io::OutputBuffers::supported(a, n, false));
}
