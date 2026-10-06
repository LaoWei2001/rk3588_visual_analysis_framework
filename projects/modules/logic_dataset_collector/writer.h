#pragma once
#include <opencv2/core.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace dataset {
struct Sample {
    std::string id, metadata, labels, annotation;
    cv::Mat image;
    bool with_annotation = false;
    bool with_metadata = false;
};
class Writer {
public:
    Writer(std::string directory, int quality, uint64_t cap_bytes, uint64_t max_samples);
    ~Writer();
    bool available(size_t image_bytes) const;
    bool enqueue(Sample sample);
    std::string error() const;
    void note_skipped() { ++skipped_; }
    uint64_t saved() const { return saved_.load(); }
    uint64_t pending() const { return pending_.load(); }
    uint64_t skipped() const { return skipped_.load(); }
private:
    void run();
    void write(const Sample& sample);
    std::string directory_;
    int quality_;
    uint64_t cap_, maximum_;
    std::atomic<uint64_t> saved_{0}, pending_{0}, skipped_{0};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Sample> queue_;
    size_t bytes_ = 0;
    bool stopping_ = false;
    std::string error_;
    std::thread worker_;
};
}
