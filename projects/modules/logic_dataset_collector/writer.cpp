#include "writer.h"
#include <opencv2/imgcodecs.hpp>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

namespace dataset {
namespace {
// 不同通道共享数据盘配额检查。永不删除已采集样本。
std::mutex disk_mutex;
const size_t queue_limit = 2, memory_limit = 64 * 1024 * 1024;
void mkdirs(const std::string& path) {
    if (path.empty() || path.front() != '/') throw std::runtime_error("采集目录目录必须是绝对路径");
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        std::string part = path.substr(0, i);
        if (mkdir(part.c_str(), 0750) != 0 && errno != EEXIST) throw std::runtime_error("无法创建采集目录目录");
        struct stat info{};
        if (lstat(part.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) throw std::runtime_error("采集目录路径不是普通目录");
    }
}
uint64_t disk_bytes(const std::string& directory) {
    uint64_t total = 0;
    DIR* dir = opendir(directory.c_str());
    if (!dir) throw std::runtime_error("无法读取采集目录目录");
    while (auto entry = readdir(dir)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string path = directory + "/" + entry->d_name;
        struct stat info{};
        if (lstat(path.c_str(), &info) != 0) continue;
        if (S_ISREG(info.st_mode)) total += info.st_size;
        else if (S_ISDIR(info.st_mode)) total += disk_bytes(path);
    }
    closedir(dir);
    return total;
}
void text_file(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), text.size()); file.close();
    if (!file) throw std::runtime_error("样本文件写入失败");
}
}
Writer::Writer(std::string directory, int quality, uint64_t cap, uint64_t maximum)
    : directory_(std::move(directory)), quality_(quality), cap_(cap), maximum_(maximum), worker_(&Writer::run, this) {}
Writer::~Writer() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    condition_.notify_one(); worker_.join();
}
bool Writer::available(size_t bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopping_ && pending_ < queue_limit && bytes_ + bytes <= memory_limit && saved_ + pending_ < maximum_;
}
bool Writer::enqueue(Sample sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t bytes = sample.image.total() * sample.image.elemSize();
    if (stopping_ || pending_ >= queue_limit || bytes_ + bytes > memory_limit || saved_ + pending_ >= maximum_) {
        ++skipped_; return false;
    }
    bytes_ += bytes; ++pending_; queue_.push_back(std::move(sample)); condition_.notify_one(); return true;
}
std::string Writer::error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
void Writer::run() {
    while (true) {
        Sample sample;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            sample = std::move(queue_.front()); queue_.pop_front();
        }
        try {
            write(sample); ++saved_;
            std::lock_guard<std::mutex> lock(mutex_); error_.clear();
        } catch (const std::exception& error) {
            ++skipped_;
            std::lock_guard<std::mutex> lock(mutex_); error_ = error.what();
        } catch (...) {
            ++skipped_;
            std::lock_guard<std::mutex> lock(mutex_); error_ = "样本保存异常";
        }
        { std::lock_guard<std::mutex> lock(mutex_); bytes_ -= sample.image.total() * sample.image.elemSize(); --pending_; }
    }
}
void Writer::write(const Sample& sample) {
    if (sample.image.empty()) throw std::runtime_error("原始画面为空");
    std::vector<unsigned char> jpeg;
    if (!cv::imencode(".jpg", sample.image, jpeg, {cv::IMWRITE_JPEG_QUALITY, quality_}))
        throw std::runtime_error("原始图片编码失败");
    std::lock_guard<std::mutex> lock(disk_mutex);
    mkdirs(directory_);
    uint64_t needed = jpeg.size();
    if (sample.with_metadata) needed += sample.metadata.size();
    if (sample.with_annotation) needed += sample.labels.size() + sample.annotation.size();
    struct statvfs disk{};
    if (statvfs(directory_.c_str(), &disk) != 0) throw std::runtime_error("无法读取采集目录剩余空间");
    uint64_t free = static_cast<uint64_t>(disk.f_bavail) * disk.f_frsize;
    if (free < needed + 256ULL * 1024 * 1024) throw std::runtime_error("数据盘剩余空间不足，停止保存；已有样本保留");
    if (disk_bytes(directory_) + needed > cap_) throw std::runtime_error("采集目录达到容量限制；请先备份并清理保存目录");
    // 同一来源帧使用唯一文件名；先写临时文件，最后提交 JPG，避免出现半张图片。
    const std::string temporary = directory_ + "/.pending_" + sample.id;
    const std::string destination = directory_ + "/" + sample.id;
    std::vector<std::string> published;
    try {
        text_file(temporary + ".jpg", std::string(reinterpret_cast<const char*>(jpeg.data()), jpeg.size()));
        if (sample.with_metadata) text_file(temporary + ".json", sample.metadata);
        if (sample.with_annotation) {
            text_file(temporary + ".txt", sample.annotation);
            text_file(temporary + ".labels.txt", sample.labels);
        }
        const auto publish = [&](const std::string& extension) {
            if (rename((temporary + extension).c_str(), (destination + extension).c_str()) != 0)
                throw std::runtime_error("无法提交采集文件");
            published.push_back(extension);
        };
        if (sample.with_metadata) publish(".json");
        if (sample.with_annotation) { publish(".txt"); publish(".labels.txt"); }
        publish(".jpg");
    } catch (...) {
        for (const char* extension : {".jpg", ".json", ".txt", ".labels.txt"}) unlink((temporary + extension).c_str());
        for (const auto& extension : published) unlink((destination + extension).c_str());
        throw;
    }
}
}
