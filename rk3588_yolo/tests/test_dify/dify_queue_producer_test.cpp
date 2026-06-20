#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "uploader/alarm_uploader.h"

struct Args {
    std::string image_path;
    std::string config_path = SOURCE_ROOT "/service/upload/config.yaml";
    std::string prompt_path = SOURCE_ROOT "/rk3588_yolo/tests/test_dify/prompt.yaml";
    std::string prompt;
    std::string event_prefix = "dify_test";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    int count = 1;
};

static std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2) {
        const char first = s.front();
        const char last = s.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

static bool parse_positive_int(const std::string& raw, int& out) {
    try {
        size_t idx = 0;
        const int value = std::stoi(raw, &idx);
        if (idx != raw.size() || value <= 0) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

static bool load_config(const std::string& config_path, Args& args) {
    std::ifstream fin(config_path);
    if (!fin.is_open()) {
        std::cerr << "[ERR] Failed to open config file: " << config_path << "\n";
        return false;
    }

    std::string current_section;
    std::string line;
    while (std::getline(fin, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }

        const std::string cleaned = trim(line);
        if (cleaned.empty()) {
            continue;
        }

        const size_t indent = line.find_first_not_of(" \t");
        if (indent == 0 && cleaned.back() == ':') {
            current_section = trim(cleaned.substr(0, cleaned.size() - 1));
            continue;
        }

        const size_t colon = cleaned.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim(cleaned.substr(0, colon));
        const std::string value = strip_quotes(trim(cleaned.substr(colon + 1)));
        if (value.empty()) {
            continue;
        }

        if (current_section == "redis") {
            if (key == "host") {
                args.redis_host = value;
            } else if (key == "port") {
                int v = 0;
                if (parse_positive_int(value, v)) {
                    args.redis_port = v;
                }
            }
            continue;
        }

        const bool test_section =
            (current_section == "producer" || current_section == "test" || current_section == "dify");
        if (!test_section && !current_section.empty()) {
            continue;
        }

        if (key == "prompt" || key == "default_prompt") {
            args.prompt = value;
        } else if (key == "event_prefix") {
            args.event_prefix = value;
        } else if (key == "count") {
            int v = 0;
            if (parse_positive_int(value, v)) {
                args.count = v;
            }
        }
    }

    if (args.redis_port <= 0) {
        std::cerr << "[ERR] Invalid redis.port from config: " << args.redis_port << "\n";
        return false;
    }
    if (args.count <= 0) {
        std::cerr << "[ERR] Invalid count from config: " << args.count << "\n";
        return false;
    }
    return true;
}

static bool load_prompt(const std::string& prompt_path, Args& args) {
    std::ifstream fin(prompt_path);
    if (!fin.is_open()) {
        std::cerr << "[ERR] Failed to open prompt file: " << prompt_path << "\n";
        return false;
    }

    std::string current_section;
    std::string line;
    while (std::getline(fin, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }

        const std::string cleaned = trim(line);
        if (cleaned.empty()) {
            continue;
        }

        const size_t indent = line.find_first_not_of(" \t");
        if (indent == 0 && cleaned.back() == ':') {
            current_section = trim(cleaned.substr(0, cleaned.size() - 1));
            continue;
        }

        const size_t colon = cleaned.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim(cleaned.substr(0, colon));
        const std::string value = strip_quotes(trim(cleaned.substr(colon + 1)));
        if (value.empty()) {
            continue;
        }

        const bool prompt_section =
            current_section.empty() || current_section == "prompt" || current_section == "test" ||
            current_section == "dify" || current_section == "producer";
        if (!prompt_section) {
            continue;
        }

        if (key == "prompt" || key == "default_prompt") {
            args.prompt = value;
            return true;
        }
    }

    std::cerr << "[ERR] No prompt found in prompt file: " << prompt_path << "\n";
    return false;
}

static void print_usage(const char* bin) {
    std::cout
        << "Usage: " << bin << " <image_path> [--config <path>]\n"
        << "  image_path             图片路径（直接跟在命令后面）\n"
        << "  --config <path>        config.yaml 路径（可选，默认自动定位）\n"
        << "  --help                 显示此帮助\n";
}

static bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (key == "--help") {
            print_usage(argv[0]);
            return false;
        } else if (key == "--image") {
            const char* v = need_value("--image");
            if (!v) return false;
            args.image_path = v;
        } else if (key == "--config") {
            const char* v = need_value("--config");
            if (!v) return false;
            args.config_path = v;
        } else if (!key.empty() && key[0] != '-') {
            if (args.image_path.empty()) {
                args.image_path = key;
            } else {
                std::cerr << "Unexpected positional arg: " << key << "\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << key << "\n";
            print_usage(argv[0]);
            return false;
        }
    }

    if (args.image_path.empty()) {
        std::cerr << "错误: 缺少图片路径\n";
        print_usage(argv[0]);
        return false;
    }

    return true;
}

static std::string build_event_id(const std::string& prefix, int index) {
    static std::mt19937 rng(std::random_device{}());
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const uint32_t r = rng();

    char buf[128] = {0};
    std::snprintf(buf, sizeof(buf), "%s_%lld_%02d_%08x", prefix.c_str(),
                  static_cast<long long>(now_ms), index, r);
    return std::string(buf);
}

static cv::Mat load_or_make_image(const std::string& image_path) {
    cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
    if (!img.empty()) {
        return img;
    }

    img = cv::Mat(480, 640, CV_8UC3, cv::Scalar(60, 80, 100));
    cv::putText(img, "DIFY TEST", cv::Point(160, 240), cv::FONT_HERSHEY_SIMPLEX,
                1.8, cv::Scalar(255, 255, 255), 3);
    std::cerr << "[WARN] Failed to read image, using generated test frame: " << image_path
              << "\n";
    return img;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    if (!load_config(args.config_path, args)) {
        return 2;
    }

    if (!load_prompt(args.prompt_path, args)) {
        return 3;
    }

    cv::Mat img = load_or_make_image(args.image_path);
    if (img.empty()) {
        std::cerr << "[ERR] Empty image, cannot continue\n";
        return 4;
    }

    std::cout << "[INFO] Using config: " << args.config_path << "\n"
              << "[INFO] Using prompt file: " << args.prompt_path << "\n"
              << "[INFO] redis=" << args.redis_host << ":" << args.redis_port
              << ", count=" << args.count
              << ", event_prefix=" << args.event_prefix << "\n";

    if (!alarm_uploader_init(args.redis_host.c_str(), args.redis_port)) {
        std::cerr << "[ERR] alarm_uploader_init failed\n";
        return 5;
    }

    int ok_count = 0;
    for (int i = 0; i < args.count; ++i) {
        std::string event_id = build_event_id(args.event_prefix, i);
        bool ok = dify_uploader_enqueue(img, args.prompt.c_str(), event_id.c_str());
        std::cout << "[" << (ok ? "OK" : "FAIL") << "] enqueue event_id=" << event_id
                  << "\n";
        if (ok) {
            ++ok_count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    alarm_uploader_deinit();

    std::cout << "Producer done. success=" << ok_count << "/" << args.count << "\n";
    return ok_count == args.count ? 0 : 5;
}
