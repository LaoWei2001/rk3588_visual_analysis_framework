// Prepared native tensors -> completed outputs, using blocking RKNN calls.
// Application pipelines and the framework's per-inference core gate are excluded.
#include "bench_options.h"
#include "inference/npu_device_owner.h"
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <random>
#include <sys/utsname.h>
#include <thread>

#ifndef NPU_BENCH_LOCK_PATH
#define NPU_BENCH_LOCK_PATH "/run/lock/rk3588-visual-npu.lock"
#endif

namespace
{
using namespace bench;
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
void check(int rc, const std::string &operation)
{
    if (rc < 0) throw std::runtime_error(operation + " failed: RKNN error " + std::to_string(rc));
}
Json tensor_json(const rknn_tensor_attr &a)
{
    std::vector<Json> dims;
    for (uint32_t i = 0; i < a.n_dims; ++i) dims.emplace_back(uint64_t(a.dims[i]));
    return Json::object({
        {"index", Json(uint64_t(a.index))}, {"name", Json(std::string(a.name, strnlen(a.name, sizeof(a.name))))},
        {"shape", Json::array(dims)}, {"type", Json(get_type_string(a.type))},
        {"layout", Json(get_format_string(a.fmt))}, {"size", Json(uint64_t(a.size))},
        {"size_with_stride", Json(uint64_t(a.size_with_stride))},
        {"w_stride", Json(uint64_t(a.w_stride))}, {"h_stride", Json(uint64_t(a.h_stride))},
        {"quantization", Json(uint64_t(a.qnt_type))}, {"zero_point", Json(double(a.zp))},
        {"scale", Json(double(a.scale))}});
}
void validate_tensor(const rknn_tensor_attr &a)
{
    if (!a.n_dims || a.n_dims > RKNN_MAX_DIMS || !a.n_elems || !std::max(a.size, a.size_with_stride))
        throw std::runtime_error("Invalid tensor; dynamic inputs need --shape");
    for (uint32_t i = 0; i < a.n_dims; ++i)
        if (!a.dims[i] || a.dims[i] > INT32_MAX) throw std::runtime_error("Unresolved shape; provide --shape");
}
struct Instance
{
    rknn_context context = 0;
    std::vector<rknn_tensor_mem *> memories;
    std::vector<rknn_tensor_attr> inputs, outputs, logical_inputs;
    std::vector<std::vector<unsigned char>> buffers;
    std::vector<rknn_output> readback;
    ~Instance() { close(false); }
    void close(bool strict)
    {
        bool failed = false;
        for (auto *memory : memories) if (rknn_destroy_mem(context, memory) < 0) failed = true;
        memories.clear();
        if (context && rknn_destroy(context) < 0) failed = true;
        context = 0;
        if (strict && failed) throw std::runtime_error("RKNN resource release failed");
    }
    rknn_tensor_attr attr(rknn_query_cmd command, unsigned index)
    {
        rknn_tensor_attr a{};
        a.index = index;
        check(rknn_query(context, command, &a, sizeof(a)), "query tensor " + std::to_string(index));
        validate_tensor(a);
        return a;
    }
    static void verify_dynamic(const rknn_tensor_attr &native, const rknn_tensor_attr &logical)
    {
        // The pinned SDK lacks CURRENT_NATIVE queries. Never bind an unverified
        // packed/default-shape buffer to a dynamically resized graph.
        if (native.fmt != logical.fmt || native.n_dims != logical.n_dims ||
            !std::equal(native.dims, native.dims + native.n_dims, logical.dims))
            throw std::runtime_error("Pinned SDK cannot verify current native dynamic shape");
    }
    void bind(rknn_tensor_attr a, const std::vector<unsigned char> *data)
    {
        const uint32_t bytes = std::max(a.size, a.size_with_stride);
        auto *memory = rknn_create_mem(context, bytes);
        if (!memory) throw std::runtime_error("rknn_create_mem failed");
        memories.push_back(memory);
        if (!memory->virt_addr || memory->size < bytes) throw std::runtime_error("Invalid tensor memory");
        std::memset(memory->virt_addr, 0, memory->size);
        if (data)
        {
            if (data->size() != bytes)
                throw std::runtime_error("Native input needs exactly " + std::to_string(bytes) +
                                         " bytes including padding");
            std::memcpy(memory->virt_addr, data->data(), bytes);
        }
        a.pass_through = 1;
        check(rknn_set_io_mem(context, memory, &a), "bind native tensor");
    }
    void setup(const Options &o, std::vector<unsigned char> &model, unsigned index,
               const std::map<unsigned, std::vector<unsigned char>> &prepared_inputs)
    {
        check(rknn_init(&context, model.data(), model.size(), 0, nullptr), "rknn_init");
        check(rknn_set_core_mask(context, static_cast<rknn_core_mask>(core_mask(o, index))), "set core mask");
        rknn_input_output_num io{};
        check(rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "query IO counts");
        if (!io.n_input || !io.n_output || io.n_input > 1024 || io.n_output > 1024)
            throw std::runtime_error("Invalid model IO counts");
        for (const auto &v : o.inputs)
            if (v.first >= io.n_input) throw std::runtime_error("--input index exceeds input count");
        for (const auto &v : o.shapes)
            if (v.first >= io.n_input) throw std::runtime_error("--shape index exceeds input count");
        bool dynamic = false;
        for (uint32_t i = 0; i < io.n_input; ++i)
        {
            rknn_tensor_attr a{};
            a.index = i;
            check(rknn_query(context, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a)), "query logical input");
            logical_inputs.push_back(a);
            rknn_input_range range{};
            range.index = i;
            if (rknn_query(context, RKNN_QUERY_INPUT_DYNAMIC_RANGE, &range, sizeof(range)) >= 0 &&
                range.shape_number > 0)
            {
                dynamic = true;
                if (!o.shapes.count(i)) throw std::runtime_error("Dynamic input " + std::to_string(i) + " needs --shape");
            }
        }
        if (!o.shapes.empty())
        {
            for (const auto &v : o.shapes)
            {
                auto &a = logical_inputs[v.first];
                a.n_dims = v.second.size();
                std::fill(std::begin(a.dims), std::end(a.dims), 0);
                std::copy(v.second.begin(), v.second.end(), a.dims);
            }
            check(rknn_set_input_shape(context, logical_inputs.data()), "set dynamic shapes");
            dynamic = true;
        }
        for (uint32_t i = 0; i < io.n_input; ++i)
        {
            logical_inputs[i] = attr(dynamic ? RKNN_QUERY_CURRENT_INPUT_ATTR : RKNN_QUERY_INPUT_ATTR, i);
            if (o.shapes.count(i))
            {
                const auto &expected = o.shapes.at(i);
                if (logical_inputs[i].n_dims != expected.size() ||
                    !std::equal(expected.begin(), expected.end(), logical_inputs[i].dims))
                    throw std::runtime_error("Runtime did not apply requested input shape");
            }
            auto a = attr(RKNN_QUERY_NATIVE_INPUT_ATTR, i);
            if (dynamic) verify_dynamic(a, logical_inputs[i]);
            inputs.push_back(a);
            bind(a, prepared_inputs.count(i) ? &prepared_inputs.at(i) : nullptr);
        }
        for (uint32_t i = 0; i < io.n_output; ++i)
        {
            auto logical = attr(dynamic ? RKNN_QUERY_CURRENT_OUTPUT_ATTR : RKNN_QUERY_OUTPUT_ATTR, i);
            if (o.io == "native")
            {
                auto a = attr(RKNN_QUERY_NATIVE_OUTPUT_ATTR, i);
                if (dynamic) verify_dynamic(a, logical);
                outputs.push_back(a);
                bind(a, nullptr);
            }
            else
            {
                if (!logical.size) throw std::runtime_error("Logical output has zero byte size");
                outputs.push_back(logical);
                buffers.emplace_back(logical.size);
            }
        }
        if (o.io == "readback")
            for (uint32_t i = 0; i < io.n_output; ++i)
            {
                rknn_output output{};
                output.index = i;
                output.is_prealloc = 1;
                output.want_float = 0;
                output.buf = buffers[i].data();
                output.size = buffers[i].size();
                readback.push_back(output);
            }
    }
    std::pair<double, double> run(unsigned timeout)
    {
        rknn_run_extend extend{};
        extend.non_block = 0;
        extend.timeout_ms = timeout;
        extend.fence_fd = -1;
        const auto begin = Clock::now();
        check(rknn_run(context, &extend), "rknn_run");
        const auto ran = Clock::now();
        if (!readback.empty())
        {
            check(rknn_outputs_get(context, readback.size(), readback.data(), nullptr), "get outputs");
            check(rknn_outputs_release(context, readback.size(), readback.data()), "release outputs");
        }
        const auto end = Clock::now();
        return {milliseconds(ran - begin), readback.empty() ? 0.0 : milliseconds(end - ran)};
    }
};
struct Statistics
{
    uint64_t count = 0, tail = 0, warmup_count = 0;
    double total_ms = 0, run_ms = 0, readback_ms = 0;
    std::vector<double> samples;
    std::mt19937_64 random{0};
    void add(double run, double readback, size_t capacity)
    {
        ++count;
        run_ms += run;
        readback_ms += readback;
        total_ms += run + readback;
        if (samples.size() < capacity) samples.push_back(run + readback);
        else
        {
            const auto position = std::uniform_int_distribution<uint64_t>(0, count - 1)(random);
            if (position < capacity) samples[position] = run + readback;
        }
    }
};
Json statistics_json(Statistics s)
{
    std::sort(s.samples.begin(), s.samples.end());
    return Json::object({
        {"completed", Json(s.count)}, {"warmup_completed", Json(s.warmup_count)},
        {"outside_window_completed", Json(s.tail)}, {"latency_samples", Json(uint64_t(s.samples.size()))},
        {"percentiles_sampled", Json(s.samples.size() < s.count)},
        {"mean_ms", Json(s.total_ms / s.count)}, {"p50_ms", Json(percentile(s.samples, .5))},
        {"p95_ms", Json(percentile(s.samples, .95))}, {"mean_run_ms", Json(s.run_ms / s.count)},
        {"mean_readback_ms", Json(s.readback_ms / s.count)}});
}
struct Round
{
    std::vector<Statistics> workers;
    double drain_ms = 0;
    std::map<std::string, std::string> before, after;
};
Round measure(const Options &o, std::vector<std::unique_ptr<Instance>> &instances)
{
    Round result;
    result.workers.resize(instances.size());
    for (size_t i = 0; i < result.workers.size(); ++i)
    {
        result.workers[i].samples.reserve(o.max_samples);
        result.workers[i].random.seed(i);
    }
    std::mutex mutex;
    std::condition_variable changed;
    enum class Phase { Ready, Warmup, Measure };
    Phase phase = Phase::Ready;
    unsigned ready = 0, warmed = 0;
    std::atomic<bool> failed{false};
    std::string error;
    Clock::time_point warmup_end, deadline;
    auto fail = [&](const std::string &message) {
        std::lock_guard<std::mutex> guard(mutex);
        if (error.empty()) error = message;
        failed.store(true);
        changed.notify_all();
    };
    std::vector<std::thread> threads;
    try
    {
        for (size_t i = 0; i < instances.size(); ++i)
            threads.emplace_back([&, i] {
                try
                {
                    auto &s = result.workers[i];
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        ++ready;
                        changed.notify_all();
                        changed.wait(lock, [&] { return phase != Phase::Ready || failed.load(); });
                        if (failed.load()) return;
                    }
                    while (!failed.load() && !interrupted && Clock::now() < warmup_end)
                    {
                        instances[i]->run(o.timeout_ms);
                        ++s.warmup_count;
                    }
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        ++warmed;
                        changed.notify_all();
                        changed.wait(lock, [&] { return phase == Phase::Measure || failed.load(); });
                        if (failed.load()) return;
                    }
                    while (!failed.load() && !interrupted && Clock::now() < deadline)
                    {
                        const auto sample = instances[i]->run(o.timeout_ms);
                        if (Clock::now() <= deadline) s.add(sample.first, sample.second, o.max_samples);
                        else ++s.tail;
                    }
                }
                catch (const std::exception &e) { fail("instance " + std::to_string(i) + ": " + e.what()); }
            });
        std::unique_lock<std::mutex> lock(mutex);
        auto wait_for = [&](unsigned &counter) {
            while (counter < instances.size() && !failed.load() && !interrupted)
                changed.wait_for(lock, std::chrono::milliseconds(50));
            if (interrupted) throw std::runtime_error("Interrupted; benchmark invalid");
            if (failed.load()) throw std::runtime_error(error);
        };
        wait_for(ready);
        warmup_end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.warmup));
        phase = Phase::Warmup;
        changed.notify_all();
        wait_for(warmed);
        result.before = environment();
        std::printf("预热完成，开始正式测量 %.3f 秒……\n", o.duration);
        std::fflush(stdout);
        deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration));
        phase = Phase::Measure;
        changed.notify_all();
        while (Clock::now() < deadline && !failed.load() && !interrupted)
            changed.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(50)));
        if (interrupted) throw std::runtime_error("Interrupted; benchmark invalid");
        if (failed.load()) throw std::runtime_error(error);
        lock.unlock();
    }
    catch (const std::exception &e) { fail(e.what()); }
    for (auto &thread : threads) thread.join();
    result.drain_ms = milliseconds(Clock::now() - deadline);
    if (interrupted) throw std::runtime_error("Interrupted; benchmark invalid");
    if (failed.load()) throw std::runtime_error(error);
    result.after = environment();
    for (const auto &s : result.workers)
        if (!s.count) throw std::runtime_error("No completed inference in window; increase --seconds");
    return result;
}
Json round_json(const Round &r, const Options &o, unsigned number)
{
    std::vector<Json> workers;
    uint64_t total = 0;
    for (size_t i = 0; i < r.workers.size(); ++i)
    {
        total += r.workers[i].count;
        workers.push_back(Json::object({{"index", Json(uint64_t(i))},
            {"core_mask", Json(uint64_t(core_mask(o, i)))}, {"metrics", statistics_json(r.workers[i])}}));
    }
    return Json::object({
        {"round", Json(uint64_t(number))}, {"window_seconds", Json(o.duration)},
        {"completed", Json(total)}, {"inferences_per_second", Json(total / o.duration)},
        {"drain_ms", Json(std::max(0.0, r.drain_ms))}, {"workers", Json::array(workers)},
        {"environment_before", environment_json(r.before)}, {"environment_after", environment_json(r.after)}});
}
void write_report(const std::string &path, const std::map<std::string, Json> &fields)
{
    if (path.empty()) return;
    std::ofstream file(path, std::ios::trunc);
    if (!file || !(file << Json::object(fields).text << '\n') || !file.flush())
        throw std::runtime_error("Cannot write JSON report: " + path);
}
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") { bench::help(); return 0; }
    Options o;
    std::map<std::string, Json> report{{"schema_version", Json(uint64_t(1))}, {"valid", Json(false)}};
    try
    {
        o = parse(argc, argv);
        report["error"] = Json("Benchmark has not completed");
        write_report(o.json_path, report); // Do not leave a previous successful report after interruption.
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        npu::DeviceOwner owner(NPU_BENCH_LOCK_PATH);
        auto model = read_binary(o.model);
        const auto model_hash = sha256(model);
        Dl_info library{};
        if (!dladdr(reinterpret_cast<void *>(rknn_init), &library) || !library.dli_fname)
            throw std::runtime_error("Cannot identify loaded RKNN Runtime");
        const std::string runtime_path = library.dli_fname;
        const auto runtime_hash = sha256(read_binary(runtime_path));
        // Cache once: all instances and the report must use the exact same bytes.
        std::map<unsigned, std::vector<unsigned char>> prepared_inputs;
        for (const auto &input : o.inputs) prepared_inputs.emplace(input.first, read_binary(input.second));
        std::vector<std::unique_ptr<Instance>> instances;
        for (unsigned i = 0; i < o.instances; ++i)
        {
            if (interrupted) throw std::runtime_error("Interrupted during initialization");
            instances.emplace_back(new Instance);
            instances.back()->setup(o, model, i, prepared_inputs);
        }
        rknn_sdk_version version{};
        check(rknn_query(instances[0]->context, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)), "query SDK version");
        std::vector<Json> inputs, outputs, masks, files;
        for (const auto &a : instances[0]->inputs) inputs.push_back(tensor_json(a));
        for (const auto &a : instances[0]->outputs) outputs.push_back(tensor_json(a));
        for (unsigned i = 0; i < o.instances; ++i) masks.emplace_back(uint64_t(core_mask(o, i)));
        for (const auto &v : o.inputs)
            files.push_back(Json::object({{"index", Json(uint64_t(v.first))}, {"path", Json(v.second)},
                                           {"sha256", Json(sha256(prepared_inputs.at(v.first)))}}));
        struct utsname system{};
        if (uname(&system)) throw std::runtime_error("uname failed");
        const auto now = std::time(nullptr);
        char date[32]{};
        std::tm utc{};
        gmtime_r(&now, &utc);
        std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc);
        report["started_utc"] = Json(date);
        report["model"] = Json::object({{"path", Json(o.model)}, {"sha256", Json(model_hash)},
                                        {"bytes", Json(uint64_t(model.size()))}});
        report["runtime"] = Json::object({{"path", Json(runtime_path)}, {"sha256", Json(runtime_hash)},
            {"api_version", Json(std::string(version.api_version, strnlen(version.api_version, sizeof(version.api_version))))},
            {"driver_version", Json(std::string(version.drv_version, strnlen(version.drv_version, sizeof(version.drv_version))))}});
        report["device"] = Json::object({{"machine", Json(system.machine)}, {"kernel", Json(system.release)},
            {"model", Json(read_text("/proc/device-tree/model"))}, {"process_status", Json(read_text("/proc/self/status"))}});
        report["config"] = Json::object({
            {"mode", Json(o.mode)}, {"io", Json(o.io)}, {"cores", Json(o.cores)}, {"core_masks", Json::array(masks)},
            {"instances", Json(uint64_t(o.instances))}, {"warmup_seconds", Json(o.warmup)},
            {"measurement_seconds", Json(o.duration)}, {"repeats", Json(uint64_t(o.repeats))},
            {"timeout_ms", Json(uint64_t(o.timeout_ms))}, {"max_samples_per_instance", Json(uint64_t(o.max_samples))},
            {"profiling", Json(false)}, {"input_source", Json(o.inputs.empty() ? "zero-filled native tensors" : "native files + zeros")},
            {"input_files", Json::array(files)}});
        report["inputs"] = Json::array(inputs);
        report["outputs"] = Json::array(outputs);
        std::printf("模型独立推理测试 | mode=%s | io=%s | instances=%u | cores=%s\n",
                    o.mode.c_str(), o.io.c_str(), o.instances, o.cores.c_str());
        std::printf("模型 SHA256: %s\nRuntime: %s | driver: %s\n", model_hash.c_str(), version.api_version, version.drv_version);
        for (size_t i = 0; i < inputs.size(); ++i) std::printf("原生输入 %zu: %s\n", i, inputs[i].text.c_str());
        for (size_t i = 0; i < outputs.size(); ++i) std::printf("输出 %zu: %s\n", i, outputs[i].text.c_str());
        std::puts("计时包含模型内部 CPU/NPU 运算，不包含图片预处理、项目调度和业务后处理。");
        std::vector<Json> rounds;
        std::vector<double> rates;
        uint64_t completed = 0;
        for (unsigned repeat = 0; repeat < o.repeats; ++repeat)
        {
            std::printf("\n第 %u/%u 轮：先预热 %.3f 秒\n", repeat + 1, o.repeats, o.warmup);
            const auto round = measure(o, instances);
            uint64_t count = 0;
            for (size_t i = 0; i < round.workers.size(); ++i)
            {
                const auto &s = round.workers[i];
                count += s.count;
                auto sorted = s.samples;
                std::sort(sorted.begin(), sorted.end());
                std::printf("  实例 %zu: %llu 次 | 平均 %.3f ms | P50 %.3f ms | P95 %.3f ms%s\n", i,
                    static_cast<unsigned long long>(s.count), s.total_ms / s.count,
                    percentile(sorted, .5), percentile(sorted, .95), s.samples.size() < s.count ? " (百分位抽样)" : "");
                std::printf("    执行 %.3f ms；输出取回及释放 %.3f ms；跨截止时间完成 %llu 次（不计吞吐）\n",
                    s.run_ms / s.count, s.readback_ms / s.count, static_cast<unsigned long long>(s.tail));
            }
            const double rate = count / o.duration;
            completed += count;
            rates.push_back(rate);
            rounds.push_back(round_json(round, o, repeat + 1));
            std::printf("  本轮总吞吐: %.3f 次/秒；窗口 %.3f 秒；收尾 %.3f ms\n", rate, o.duration, std::max(0.0, round.drain_ms));
            print_environment("  测量开始环境：", round.before);
            print_environment("  测量结束环境：", round.after);
            report["rounds"] = Json::array(rounds);
            write_report(o.json_path, report);
        }
        for (auto &instance : instances) instance->close(true);
        const double total_seconds = o.duration * o.repeats;
        report["summary"] = Json::object({{"completed", Json(completed)}, {"measurement_seconds", Json(total_seconds)},
            {"inferences_per_second", Json(completed / total_seconds)},
            {"min_round_inferences_per_second", Json(*std::min_element(rates.begin(), rates.end()))},
            {"max_round_inferences_per_second", Json(*std::max_element(rates.begin(), rates.end()))},
            {"failed", Json(uint64_t(0))}});
        report["valid"] = Json(true);
        report.erase("error");
        write_report(o.json_path, report);
        std::printf("\n测试有效：总吞吐 %.3f 次/秒；各轮范围 %.3f..%.3f；失败 0\n",
            completed / total_seconds, *std::min_element(rates.begin(), rates.end()), *std::max_element(rates.begin(), rates.end()));
        if (!o.json_path.empty()) std::printf("报告：%s\n", o.json_path.c_str());
        return 0;
    }
    catch (const std::exception &e)
    {
        report["valid"] = Json(false);
        report["error"] = Json(e.what());
        try { write_report(o.json_path, report); }
        catch (const std::exception &write_error) { std::fprintf(stderr, "%s\n", write_error.what()); }
        std::fprintf(stderr, "测试无效：%s\n", e.what());
        return interrupted ? 130 : 1;
    }
}
