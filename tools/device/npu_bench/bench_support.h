#pragma once
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <glob.h>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace bench
{
using Clock = std::chrono::steady_clock;
inline double milliseconds(Clock::duration value)
{
    return std::chrono::duration<double, std::milli>(value).count();
}
inline std::vector<unsigned char> read_binary(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot read file: " + path);
    const auto size = file.tellg();
    if (size <= 0 || uint64_t(size) > UINT32_MAX)
        throw std::runtime_error("File must contain 1..4294967295 bytes: " + path);
    std::vector<unsigned char> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), data.size()))
        throw std::runtime_error("Cannot read complete file: " + path);
    return data;
}
inline std::string sha256(const std::vector<unsigned char> &data)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(data.data(), data.size(), digest)) throw std::runtime_error("SHA256 failed");
    std::ostringstream out;
    for (auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return out.str();
}

// Write-only JSON: reports do not depend on the application's configuration code.
struct Json
{
    std::string text = "null";
    Json() = default;
    explicit Json(const std::string &value)
    {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : value)
        {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
            else out << c;
        }
        out << '"';
        text = out.str();
    }
    explicit Json(const char *value) : Json(std::string(value)) {}
    explicit Json(bool value) : text(value ? "true" : "false") {}
    explicit Json(uint64_t value) : text(std::to_string(value)) {}
    explicit Json(double value)
    {
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite JSON number");
        std::ostringstream out;
        out << std::setprecision(15) << value;
        text = out.str();
    }
    static Json object(const std::map<std::string, Json> &values)
    {
        Json result;
        result.text = "{";
        for (const auto &entry : values)
        {
            if (result.text.size() > 1) result.text += ",";
            result.text += Json(entry.first).text + ":" + entry.second.text;
        }
        result.text += "}";
        return result;
    }
    static Json array(const std::vector<Json> &values)
    {
        Json result;
        result.text = "[";
        for (const auto &value : values)
        {
            if (result.text.size() > 1) result.text += ",";
            result.text += value.text;
        }
        result.text += "]";
        return result;
    }
};
inline std::string read_text(const std::string &path)
{
    std::ifstream file(path);
    if (!file) return "unavailable";
    std::string value((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\0' || value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}
inline std::map<std::string, std::string> environment()
{
    std::map<std::string, std::string> result;
    for (const char *pattern : {
             "/sys/devices/system/cpu/cpufreq/policy*/scaling_governor",
             "/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq",
             "/sys/class/devfreq/*/governor", "/sys/class/devfreq/*/cur_freq",
             "/sys/class/thermal/thermal_zone*/type", "/sys/class/thermal/thermal_zone*/temp"})
    {
        glob_t matches{};
        if (glob(pattern, 0, nullptr, &matches) == 0)
            for (size_t i = 0; i < matches.gl_pathc; ++i)
                result[matches.gl_pathv[i]] = read_text(matches.gl_pathv[i]);
        globfree(&matches);
    }
    result["loadavg"] = read_text("/proc/loadavg");
    return result;
}
inline Json environment_json(const std::map<std::string, std::string> &values)
{
    std::map<std::string, Json> result;
    for (const auto &entry : values) result.emplace(entry.first, Json(entry.second));
    return Json::object(result);
}
inline void print_environment(const char *label, const std::map<std::string, std::string> &values)
{
    std::printf("%s\n", label);
    for (const auto &entry : values)
    {
        const auto policy = entry.first.find("policy");
        if (entry.first.find("/scaling_cur_freq") != std::string::npos && policy != std::string::npos)
            std::printf("  CPU %s: %s kHz\n",
                        entry.first.substr(policy, entry.first.find('/', policy) - policy).c_str(), entry.second.c_str());
        else if (entry.first.find("/cur_freq") != std::string::npos &&
                 (entry.first.find("npu") != std::string::npos || entry.first.find("dmc") != std::string::npos))
            std::printf("  %s频率: %s Hz\n",
                        entry.first.find("npu") != std::string::npos ? "NPU" : "DDR", entry.second.c_str());
    }
    for (const auto &entry : values)
        if (entry.first.find("/temp") != std::string::npos)
        {
            const auto type = values.find(entry.first.substr(0, entry.first.size() - 4) + "type");
            if (type != values.end() && type->second.find("npu") != std::string::npos)
            {
                try { std::printf("  NPU 温度: %.1f C\n", std::stod(entry.second) / 1000.0); }
                catch (const std::exception &) { std::printf("  NPU 温度: unavailable\n"); }
            }
        }
}
inline bool same_file(const std::string &a, const std::string &b)
{
    struct stat left{}, right{};
    return stat(a.c_str(), &left) == 0 && stat(b.c_str(), &right) == 0 &&
           left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
inline double percentile(const std::vector<double> &sorted, double fraction)
{
    if (sorted.empty()) throw std::runtime_error("No latency samples");
    const double position = fraction * (sorted.size() - 1);
    const size_t lo = static_cast<size_t>(position);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (position - lo);
}
} // namespace bench
