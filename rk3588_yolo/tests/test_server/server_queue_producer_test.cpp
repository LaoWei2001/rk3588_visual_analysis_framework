/**
 * @file server_queue_producer_test.cpp
 * @brief 测试 alarm_uploader_enqueue()：向 Redis server_queue 推送报警数据
 *
 * 功能：
 *   1. 读取 server_upload/config.yaml 获取 Redis 连接信息
 *   2. 加载（或自动生成）draw / raw 两张测试图片
 *   3. 多次调用 alarm_uploader_enqueue()，模拟视觉程序的真实上报行为
 *   4. 等待内部异步 worker 线程将数据 RPUSH 到 Redis server_queue
 *
 * 与实际项目的对应关系：
 *   C++ 侧 alarm_uploader_enqueue()
 *     → Redis server_queue
 *       → Python upload/server_upload/main.py (ServerUploader.upload())
 *         → HTTP POST 到业务服务器
 *
 * 用法：
 *   ./server_queue_producer_test [--image-draw <path>] [--image-raw <path>]
 *                                [--config <path>] [--help]
 *
 * 参数说明（也可在 config.yaml 的 [test] 节配置）：
 *   --image-draw  带标注框的报警截图（可省略，省略时自动生成测试图）
 *   --image-raw   原始未标注图（可省略，省略时自动生成测试图）
 *   --config      server_upload/config.yaml 路径
 *                 默认值: /userdata/rk3588/upload/server_upload/config.yaml
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "../../src/uploader/alarm_uploader.h"

// ─────────────────────────── 参数结构 ─────────────────────────────────────

struct Args {
    std::string image_draw_path;
    std::string image_raw_path;
    std::string config_path  = "/userdata/rk3588/upload/server_upload/config.yaml";
    std::string alarm_type   = "person_alarm";
    std::string redis_host   = "127.0.0.1";
    std::string server_url;          // 仅用于打印提示，不参与推送
    int         redis_port   = 6379;
    int         camera_id    = 1;
    int         count        = 3;
};

// ─────────────────────────── 简易 YAML 解析 ───────────────────────────────

static std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last  = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2) {
        const char f = s.front(), l = s.back();
        if ((f == '"' && l == '"') || (f == '\'' && l == '\''))
            return s.substr(1, s.size() - 2);
    }
    return s;
}

static bool parse_positive_int(const std::string& raw, int& out) {
    try {
        size_t idx = 0;
        const int v = std::stoi(raw, &idx);
        if (idx != raw.size() || v <= 0) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * 从 server_upload/config.yaml 读取以下字段：
 *   redis.host / redis.port            → Redis 连接
 *   server.url                         → 仅打印提示
 *   test.count / test.camera_id / test.alarm_type  → 可选测试覆盖
 */
static bool load_config(const std::string& path, Args& args) {
    std::ifstream fin(path);
    if (!fin.is_open()) {
        std::cerr << "[ERR] Cannot open config: " << path << "\n";
        return false;
    }

    std::string section, line;
    while (std::getline(fin, line)) {
        // 去注释
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        const std::string cleaned = trim(line);
        if (cleaned.empty()) continue;

        // 检测 section 行（无缩进，以冒号结尾，冒号后无值）
        const size_t indent = line.find_first_not_of(" \t");
        const size_t colon  = cleaned.find(':');
        if (colon == std::string::npos) continue;

        const std::string key_raw   = trim(cleaned.substr(0, colon));
        const std::string val_raw   = strip_quotes(trim(cleaned.substr(colon + 1)));

        if (indent == 0 && val_raw.empty()) {
            section = key_raw;
            continue;
        }

        if (section == "redis") {
            if      (key_raw == "host") args.redis_host = val_raw;
            else if (key_raw == "port") { int v = 0; if (parse_positive_int(val_raw, v)) args.redis_port = v; }
        } else if (section == "server") {
            if (key_raw == "url") args.server_url = val_raw;
        } else if (section == "test") {
            // 可选测试配置节，不影响 server_upload/config.yaml 的正常使用
            if (key_raw == "count")      { int v = 0; if (parse_positive_int(val_raw, v)) args.count = v; }
            if (key_raw == "camera_id")  { int v = 0; if (std::stoi(val_raw, nullptr) >= 0) args.camera_id = std::stoi(val_raw); }
            if (key_raw == "alarm_type" && !val_raw.empty()) args.alarm_type = val_raw;
        }
    }

    if (args.redis_port <= 0) {
        std::cerr << "[ERR] Invalid redis.port: " << args.redis_port << "\n";
        return false;
    }
    return true;
}

// ─────────────────────────── 命令行解析 ───────────────────────────────────

static void print_usage(const char* bin) {
    std::cout
        << "Usage: " << bin << " [options]\n"
        << "  --image-draw <path>    带标注框的报警图（省略则自动生成）\n"
        << "  --image-raw  <path>    原始未标注图（省略则自动生成）\n"
        << "  --config     <path>    server_upload/config.yaml 路径\n"
        << "  --help                 显示帮助\n";
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

        if      (key == "--help")       { print_usage(argv[0]); return false; }
        else if (key == "--image-draw") { const char* v = need_value("--image-draw"); if (!v) return false; args.image_draw_path = v; }
        else if (key == "--image-raw")  { const char* v = need_value("--image-raw");  if (!v) return false; args.image_raw_path  = v; }
        else if (key == "--config")     { const char* v = need_value("--config");     if (!v) return false; args.config_path     = v; }
        else {
            std::cerr << "Unknown option: " << key << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

// ─────────────────────────── 图像辅助函数 ─────────────────────────────────

/**
 * 加载图片，或自动生成一张填充了测试文字的占位图。
 * 与 dify_queue_producer_test.cpp 保持相同的降级逻辑。
 */
static cv::Mat load_or_make_image(const std::string& path,
                                  const std::string& label,
                                  const cv::Scalar& bg_color) {
    if (!path.empty()) {
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (!img.empty()) return img;
        std::cerr << "[WARN] Failed to read image: " << path
                  << ", falling back to generated frame.\n";
    }
    cv::Mat img(480, 640, CV_8UC3, bg_color);
    cv::putText(img, label, cv::Point(80, 240),
                cv::FONT_HERSHEY_SIMPLEX, 1.6, cv::Scalar(255, 255, 255), 3);
    return img;
}

/**
 * 在 img_draw 上叠加一个红色检测框和文字，模拟视觉分析结果标注。
 */
static void draw_detection_box(cv::Mat& img, const std::string& alarm_type, int camera_id) {
    // 模拟一个中央检测框
    const int w = img.cols, h = img.rows;
    cv::Rect box(w / 4, h / 4, w / 2, h / 2);
    cv::rectangle(img, box, cv::Scalar(0, 0, 255), 2);

    char label[128];
    std::snprintf(label, sizeof(label), "CAM%d %s", camera_id, alarm_type.c_str());
    cv::putText(img, label, cv::Point(box.x, box.y - 8),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
}

// ─────────────────────────── 主函数 ───────────────────────────────────────

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;
    if (!load_config(args.config_path, args)) return 2;

    // 准备 draw 图（带检测框标注）
    cv::Mat img_draw = load_or_make_image(
        args.image_draw_path,
        "DRAW FRAME",
        cv::Scalar(40, 60, 80)   // 深色背景
    );
    // 若没有提供图片（自动生成），模拟叠加检测框
    if (args.image_draw_path.empty()) {
        draw_detection_box(img_draw, args.alarm_type, args.camera_id);
    }

    // 准备 raw 图（原始帧，无标注）
    cv::Mat img_raw = load_or_make_image(
        args.image_raw_path,
        "RAW  FRAME",
        cv::Scalar(60, 80, 40)   // 稍有区别的背景色，便于区分
    );

    std::cout
        << "================================================\n"
        << "  server_queue 生产者测试\n"
        << "  config      : " << args.config_path       << "\n"
        << "  redis       : " << args.redis_host << ":" << args.redis_port << "\n"
        << "  server_url  : " << (args.server_url.empty() ? "(未配置)" : args.server_url) << "\n"
        << "  camera_id   : " << args.camera_id          << "\n"
        << "  alarm_type  : " << args.alarm_type          << "\n"
        << "  count       : " << args.count               << "\n"
        << "================================================\n";

    // 初始化：建立 Redis 连接 + 启动异步 upload worker 线程
    if (!alarm_uploader_init(args.redis_host.c_str(), args.redis_port)) {
        std::cerr << "[ERR] alarm_uploader_init failed (Redis 连接失败？)\n";
        return 3;
    }

    int ok_count = 0;
    for (int i = 0; i < args.count; ++i) {
        // alarm_uploader_enqueue 是非阻塞的：
        //   - 克隆图像、记录时间戳后入队
        //   - 实际的 JPEG 编码 + Redis RPUSH 由 upload worker 线程异步完成
        //
        // 推送到 Redis server_queue 的 JSON 格式（由 alarm_uploader.cpp 决定）：
        //   {
        //     "timestamp":    <unix time>,
        //     "camera_id":    <int>,
        //     "alarm_type":   <string>,
        //     "snapTime":     "YYYY-MM-DD HH:MM:SS",
        //     "endTime":      "YYYY-MM-DD HH:MM:SS",
        //     "base64Data":   <img_draw JPEG base64>,
        //     "base64DataRaw":<img_raw  JPEG base64>
        //   }
        bool ok = alarm_uploader_enqueue(img_draw, img_raw, args.camera_id, args.alarm_type);
        std::cout << "[" << (ok ? "OK  " : "FAIL") << "] enqueue #" << (i + 1)
                  << " camera_id=" << args.camera_id
                  << " alarm_type=" << args.alarm_type << "\n";
        if (ok) ++ok_count;

        // 轻微间隔，避免同一毫秒内的时间戳完全相同
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // deinit 会等待 worker 线程将剩余队列全部 RPUSH 到 Redis 后才 join
    // 所以此处无需额外 sleep
    alarm_uploader_deinit();

    std::cout << "\nProducer done. enqueue success=" << ok_count
              << "/" << args.count << "\n";

    if (ok_count < args.count) {
        std::cerr << "[WARN] " << (args.count - ok_count)
                  << " message(s) failed to enqueue (queue full or Redis unavailable).\n";
        return 4;
    }
    return 0;
}
