// Test-only Runtime: controllable delays, native layouts, failures and cleanup.
#include <rknn_api.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct Model { std::vector<rknn_tensor_attr> inputs; unsigned runs = 0, allocations = 0; };
std::map<rknn_context, Model> models;
std::mutex mutex;
rknn_context next_id = 1;
std::atomic<unsigned> active{0};
std::string env(const char *key) { const char *v = std::getenv(key); return v ? v : ""; }
bool scenario(const char *s) { return env("FAKE_SCENARIO") == s; }
bool fail(const char *s) { return env("FAKE_FAIL") == s; }
void trace(const std::string &s)
{
    const auto file = env("FAKE_TRACE");
    if (!file.empty()) { std::lock_guard<std::mutex> guard(mutex); std::ofstream(file, std::ios::app) << s << '\n'; }
}
void sleep_us(const char *name, unsigned fallback)
{
    const auto v = env(name);
    std::this_thread::sleep_for(std::chrono::microseconds(v.empty() ? fallback : std::stoul(v)));
}
rknn_tensor_attr input(unsigned index)
{
    rknn_tensor_attr a{};
    a.index = index;
    std::snprintf(a.name, sizeof(a.name), "input%u", index);
    a.type = index ? RKNN_TENSOR_INT64 : RKNN_TENSOR_FLOAT16;
    a.fmt = index ? RKNN_TENSOR_UNDEFINED : RKNN_TENSOR_NHWC;
    a.n_dims = index ? 2 : 4;
    a.dims[0] = 1;
    if (index) a.dims[1] = 4;
    else { a.dims[1] = 2; a.dims[2] = 2; a.dims[3] = 3; }
    a.n_elems = index ? 4 : 12;
    a.size = index ? 32 : 24;
    a.size_with_stride = 32;
    return a;
}
}
extern "C" {
int rknn_init(rknn_context *ctx, void *, uint32_t size, uint32_t flags, rknn_init_extend *)
{
    if (!size || flags || fail("init")) return -6;
    sleep_us("FAKE_INIT_US", 0);
    { std::lock_guard<std::mutex> guard(mutex);
      *ctx = next_id++; models[*ctx] = Model{};
      for (unsigned i = 0; i < (scenario("multi") ? 2u : 1u); ++i) models[*ctx].inputs.push_back(input(i)); }
    trace("init");
    return 0;
}
int rknn_set_core_mask(rknn_context, rknn_core_mask mask)
{
    trace("core " + std::to_string(mask));
    return fail("core") ? -5 : 0;
}
int rknn_query(rknn_context ctx, rknn_query_cmd cmd, void *info, uint32_t)
{
    if (fail("query") || (fail("native_query") && cmd == RKNN_QUERY_NATIVE_INPUT_ATTR)) return -1;
    if (cmd == RKNN_QUERY_IN_OUT_NUM) {
        auto *io = static_cast<rknn_input_output_num *>(info);
        io->n_input = models.at(ctx).inputs.size(); io->n_output = scenario("multi") ? 2 : 1; return 0;
    }
    if (cmd == RKNN_QUERY_SDK_VERSION) {
        auto *v = static_cast<rknn_sdk_version *>(info);
        std::strcpy(v->api_version, "fake-runtime"); std::strcpy(v->drv_version, "fake-driver"); return 0;
    }
    if (cmd == RKNN_QUERY_INPUT_DYNAMIC_RANGE) {
        if (!scenario("dynamic") && !scenario("stale")) return -1;
        static_cast<rknn_input_range *>(info)->shape_number = 2; return 0;
    }
    auto *a = static_cast<rknn_tensor_attr *>(info);
    const auto index = a->index;
    if (cmd == RKNN_QUERY_INPUT_ATTR || cmd == RKNN_QUERY_CURRENT_INPUT_ATTR ||
        cmd == RKNN_QUERY_NATIVE_INPUT_ATTR) {
        *a = scenario("stale") && cmd == RKNN_QUERY_NATIVE_INPUT_ATTR ? input(index) : models.at(ctx).inputs.at(index);
        return 0;
    }
    if (cmd == RKNN_QUERY_OUTPUT_ATTR || cmd == RKNN_QUERY_CURRENT_OUTPUT_ATTR ||
        cmd == RKNN_QUERY_NATIVE_OUTPUT_ATTR) {
        *a = {};
        a->index = index; std::snprintf(a->name, sizeof(a->name), "output%u", index);
        a->type = RKNN_TENSOR_FLOAT16; a->fmt = RKNN_TENSOR_NCHW;
        a->n_dims = 4; a->dims[0] = 1; a->dims[1] = 1; a->dims[2] = 2; a->dims[3] = 2;
        a->n_elems = 4; a->size = 8; a->size_with_stride = 8;
        if (cmd == RKNN_QUERY_NATIVE_OUTPUT_ATTR && !scenario("dynamic") && !scenario("stale")) {
            a->fmt = RKNN_TENSOR_NC1HWC2; a->n_dims = 5; a->dims[4] = 8;
            a->n_elems = 32; a->size = 64; a->size_with_stride = 64;
        }
        return 0;
    }
    return -1;
}
int rknn_set_input_shape(rknn_context ctx, rknn_tensor_attr *attrs)
{
    trace("shape");
    if (fail("shape")) return -5;
    auto &model = models.at(ctx);
    for (size_t i = 0; i < model.inputs.size(); ++i) {
        auto a = attrs[i];
        a.n_elems = 1;
        for (uint32_t j = 0; j < a.n_dims; ++j) a.n_elems *= a.dims[j];
        a.size = a.n_elems * (a.type == RKNN_TENSOR_FLOAT16 ? 2 : 8);
        a.size_with_stride = a.size;
        model.inputs[i] = a;
    }
    return 0;
}
rknn_tensor_mem *rknn_create_mem(rknn_context ctx, uint32_t size)
{
    auto &m = models.at(ctx);
    ++m.allocations;
    if (fail("create") || (fail("create2") && m.allocations == 2)) return nullptr;
    auto *memory = new rknn_tensor_mem{};
    memory->virt_addr = std::malloc(size); memory->size = size;
    trace("alloc " + std::to_string(size));
    return memory;
}
int rknn_set_io_mem(rknn_context, rknn_tensor_mem *memory, rknn_tensor_attr *a)
{
    trace("bind " + std::string(a->name) + " " + std::to_string(a->type) + " " + std::to_string(a->fmt));
    if (fail("bind") || !a->pass_through || memory->size < a->size_with_stride) return -5;
    if (!std::strncmp(a->name, "input0", 6) && !env("FAKE_INPUT_BYTE").empty()) {
        const auto expected = std::stoul(env("FAKE_INPUT_BYTE"));
        const auto *bytes = static_cast<unsigned char *>(memory->virt_addr);
        for (unsigned i = 0; i < a->size_with_stride; ++i) if (bytes[i] != expected) return -8;
    }
    return 0;
}
int rknn_run(rknn_context ctx, rknn_run_extend *extend)
{
    if (!extend || extend->non_block || extend->timeout_ms <= 0 || extend->fence_fd != -1) return -5;
    auto &m = models.at(ctx);
    ++m.runs;
    if (fail("run") || (fail("run_later") && m.runs > 2)) return -1;
    if (active.fetch_add(1) > 0) trace("overlap");
    if (m.runs == 1 && !env("FAKE_FIRST_RUN_US").empty()) sleep_us("FAKE_FIRST_RUN_US", 0);
    else sleep_us("FAKE_RUN_US", 5000);
    active.fetch_sub(1);
    trace("run");
    return 0;
}
int rknn_outputs_get(rknn_context, uint32_t count, rknn_output *out, rknn_output_extend *)
{
    trace("get");
    if (fail("get")) return -9;
    for (unsigned i = 0; i < count; ++i)
        if (out[i].want_float || !out[i].is_prealloc || !out[i].buf || out[i].size < 8) return -5;
    sleep_us("FAKE_GET_US", 2000);
    return 0;
}
int rknn_outputs_release(rknn_context, uint32_t, rknn_output *)
{
    trace("release");
    return fail("release") ? -1 : 0;
}
int rknn_destroy_mem(rknn_context, rknn_tensor_mem *memory)
{
    std::free(memory->virt_addr); delete memory; trace("free");
    return fail("free") ? -1 : 0;
}
int rknn_destroy(rknn_context ctx)
{
    models.erase(ctx); trace("destroy");
    return fail("destroy") ? -1 : 0;
}
}
