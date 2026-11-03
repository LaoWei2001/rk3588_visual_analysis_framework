#pragma once
#include "bench_support.h"
#include <rknn_api.h>
#include <climits>

namespace bench
{
struct Options
{
    std::string model, mode = "latency", cores, io = "native", json_path;
    unsigned instances = 0, repeats = 3, timeout_ms = 10000;
    size_t max_samples = 100000;
    double warmup = 3, duration = 15;
    std::map<unsigned, std::string> inputs;
    std::map<unsigned, std::vector<uint32_t>> shapes;
};
inline uint64_t integer(const std::string &s, uint64_t lo, uint64_t hi)
{
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected an integer: " + s);
    const auto n = std::stoull(s);
    if (n < lo || n > hi) throw std::runtime_error("Integer outside supported range: " + s);
    return n;
}
inline double seconds(const std::string &s, bool allow_zero)
{
    size_t consumed = 0;
    const double n = std::stod(s, &consumed);
    if (consumed != s.size() || !std::isfinite(n) || n < (allow_zero ? 0.0 : .001) || n > 86400)
        throw std::runtime_error("Invalid duration: " + s);
    return n;
}
inline void help()
{
    std::puts(
        "用法: ./npu_bench model.rknn [选项]\n"
        "默认: 单实例延迟，核心 0，原生输入输出；每轮预热 3 秒、测量 15 秒，重复 3 轮。\n"
        "  --mode latency|throughput   单实例延迟 / 多实例吞吐\n"
        "  --instances N               吞吐模式默认 3；延迟模式必须为 1\n"
        "  --cores 0|1|2|0,1|0,1,2|auto|round-robin\n"
        "                              默认延迟模式 0，吞吐模式 round-robin\n"
        "  --warmup SECONDS            每轮预热秒数，允许 0\n"
        "  --seconds SECONDS           每轮正式测量秒数，默认 15\n"
        "  --repeats N                 重复轮数，默认 3\n"
        "  --io native|readback        原生输出就绪 / 再取回普通格式输出\n"
        "  --input INDEX:FILE          原生输入二进制，可重复；未指定的输入缓冲清零\n"
        "  --shape INDEX:1x3x640x640    动态输入的逻辑形状，可重复\n"
        "  --timeout-ms N              阻塞推理超时，默认 10000 毫秒\n"
        "  --max-samples N             每实例延迟样本上限，默认 100000\n"
        "  --json FILE                 保存配置、结果和设备环境\n"
        "  --help                      查看帮助\n"
        "旧数字位置参数已取消；模型内部 CPU 算子计入推理，不包含应用管线。");
}
inline Options parse(int argc, char **argv)
{
    if (argc < 2) throw std::runtime_error("Missing model file; use --help");
    Options o;
    o.model = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        const std::string key = argv[i];
        if (key.compare(0, 2, "--") != 0)
            throw std::runtime_error("Old positional arguments unsupported; use --help: " + key);
        if (++i == argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[i];
        if (key == "--mode") o.mode = value;
        else if (key == "--cores") o.cores = value;
        else if (key == "--io") o.io = value;
        else if (key == "--json") o.json_path = value;
        else if (key == "--instances") o.instances = integer(value, 1, 64);
        else if (key == "--repeats") o.repeats = integer(value, 1, 1000);
        else if (key == "--timeout-ms") o.timeout_ms = integer(value, 1, 3600000);
        else if (key == "--max-samples") o.max_samples = integer(value, 1, 10000000);
        else if (key == "--warmup") o.warmup = seconds(value, true);
        else if (key == "--seconds") o.duration = seconds(value, false);
        else if (key == "--input" || key == "--shape")
        {
            const auto colon = value.find(':');
            if (colon == std::string::npos) throw std::runtime_error("Expected INDEX:VALUE for " + key);
            const unsigned index = integer(value.substr(0, colon), 0, 1023);
            const auto rest = value.substr(colon + 1);
            if (rest.empty()) throw std::runtime_error("Empty value for " + key);
            if (key == "--input")
            {
                if (!o.inputs.emplace(index, rest).second) throw std::runtime_error("Duplicate input index");
            }
            else
            {
                std::vector<uint32_t> dims;
                std::istringstream stream(rest);
                std::string dim;
                while (std::getline(stream, dim, 'x')) dims.push_back(integer(dim, 1, INT32_MAX));
                if (dims.empty() || dims.size() > RKNN_MAX_DIMS || rest.back() == 'x')
                    throw std::runtime_error("Invalid shape");
                if (!o.shapes.emplace(index, dims).second) throw std::runtime_error("Duplicate shape index");
            }
        }
        else throw std::runtime_error("Unknown option: " + key);
    }
    if (o.mode != "latency" && o.mode != "throughput") throw std::runtime_error("Invalid --mode");
    if (o.io != "native" && o.io != "readback") throw std::runtime_error("Invalid --io");
    if (!o.instances) o.instances = o.mode == "latency" ? 1 : 3;
    if (o.mode == "latency" && o.instances != 1) throw std::runtime_error("latency requires one instance");
    if (o.cores.empty()) o.cores = o.mode == "latency" ? "0" : "round-robin";
    const std::vector<std::string> cores{"0", "1", "2", "0,1", "0,1,2", "auto", "round-robin"};
    if (std::find(cores.begin(), cores.end(), o.cores) == cores.end()) throw std::runtime_error("Invalid --cores");
    if (!o.json_path.empty())
    {
        if (same_file(o.model, o.json_path)) throw std::runtime_error("Report must not overwrite model");
        for (const auto &input : o.inputs)
            if (same_file(input.second, o.json_path)) throw std::runtime_error("Report must not overwrite input");
    }
    return o;
}
inline int core_mask(const Options &o, unsigned i)
{
    if (o.cores == "round-robin") return 1 << (i % 3);
    if (o.cores == "auto") return RKNN_NPU_CORE_AUTO;
    int mask = 0;
    for (char c : o.cores) if (c >= '0' && c <= '2') mask |= 1 << (c - '0');
    return mask;
}
}
