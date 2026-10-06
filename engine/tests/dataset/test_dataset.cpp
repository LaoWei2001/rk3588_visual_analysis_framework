#include "rules.h"
#include "writer.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <opencv2/imgcodecs.hpp>
using namespace dataset;
std::string rule(const std::string& condition) { return "[{\"id\":\"r\",\"name\":\"r\",\"condition\":" + condition + "}]"; }
std::string absent(const std::string& c) { return "{\"class\":\"" + c + "\",\"min_confidence\":0.3,\"count\":{\"op\":\"==\",\"value\":0}}"; }
bool rejected(const std::string& text, std::vector<std::string> allowed = {}, double threshold = 0.3) {
    try { parse_rules(text, {"x", "y"}, allowed, threshold); return false; } catch (const std::runtime_error&) { return true; }
}
int main() {
    const auto any = parse_rules(rule("{\"any\":[" + absent("x") + "," + absent("y") + "]}"), {"x", "y"}, {}, 0.3);
    const auto all = parse_rules(rule("{\"all\":[" + absent("x") + "," + absent("y") + "]}"), {"x", "y"}, {}, 0.3);
    for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) {
        std::vector<Detection> ds; if (x) ds.push_back({"x", 0.8}); if (y) ds.push_back({"y", 0.8});
        assert(any[0].condition.matches(ds) == (!x || !y));
        assert(all[0].condition.matches(ds) == (!x && !y));
    }
    auto negated = parse_rules(rule("{\"not\":" + absent("x") + "}"), {"x"}, {}, 0.3);
    assert(!negated[0].condition.matches({{"x", 0.2}}));
    assert(negated[0].condition.matches({{"x", 0.9}}));
    auto count = parse_rules(rule("{\"class\":\"x\",\"min_confidence\":0.5,\"count\":{\"op\":\">=\",\"value\":2}}"), {"x"}, {}, 0.3);
    assert(!count[0].condition.matches({{"x", 0.8}, {"x", 0.4}}));
    assert(count[0].condition.matches({{"x", 0.8}, {"x", 0.6}}));
    assert(rejected(rule(absent("unknown"))));
    assert(rejected(rule(absent("x")), {"y"}));
    assert(rejected(rule(absent("x")), {}, 0.5));
    assert(rejected(rule("{\"any\":[]}")));
    assert(rejected(rule("{\"all\":[" + absent("x") + "],\"not\":" + absent("x") + "}")));
    assert(rejected(rule("{\"class\":\"x\",\"count\":{\"op\":\"==\",\"value\":-1}}")));
    assert(rejected(rule(absent("x")) + " garbage"));
    std::string deep = absent("x"); for (int i = 0; i < 10; ++i) deep = "{\"not\":" + deep + "}";
    assert(rejected(rule(deep)));
    // ALL 必须完整验证，不能因为第一项肯定满足而接受未知类别。
    assert(rejected(rule("{\"all\":[" + absent("x") + "," + absent("unknown") + "]}")));
    Gate gate(2);
    assert(gate.update(1, 1000, {true, false}, 200, 1000, 2000, false).empty());
    assert(gate.update(1, 1300, {true, false}, 200, 1000, 2000, false).empty()); // duplicate
    assert(gate.update(2, 1200, {true, false}, 200, 1000, 2000, false) == std::vector<size_t>{0});
    assert(gate.update(3, 2000, {true, true}, 200, 1000, 2000, false).empty());
    assert(gate.update(4, 2200, {true, true}, 200, 1000, 2000, false) == (std::vector<size_t>{0, 1}));
    assert(gate.update(5, 5000, {true, true}, 200, 1000, 2000, false).empty()); // gap resets confirm
    Gate enter(1);
    assert(!enter.update(1, 1000, {true}, 0, 1000, 2000, true).empty());
    assert(enter.update(2, 1200, {true}, 0, 1000, 2000, true).empty());
    assert(enter.update(3, 1400, {false}, 0, 1000, 2000, true).empty());
    assert(enter.update(4, 1500, {true}, 0, 1000, 2000, true).empty());
    assert(!enter.update(5, 2000, {true}, 0, 1000, 2000, true).empty()); // entry retained during cooldown
    char path[] = "/userdata/tmp/dataset-test-XXXXXX";
    assert(mkdtemp(path));
    {
        Writer writer(path, 95, 1024 * 1024, 1);
        Sample sample; sample.id = "test"; sample.metadata = "{}"; sample.labels = "x\ny\n";
        sample.annotation = "0 0.5 0.5 0.2 0.2\n"; sample.with_annotation = true; sample.with_metadata = true;
        sample.image = cv::Mat(720, 1280, CV_8UC3, cv::Scalar(30, 80, 110));
        assert(writer.enqueue(sample));
        assert(!writer.enqueue(sample)); // maximum includes pending reservations
        assert(!writer.available(1280 * 720 * 3));
    } // destructor must drain pending writes
    auto image = cv::imread(std::string(path) + "/test.jpg");
    assert(image.cols == 1280 && image.rows == 720);
    assert(std::ifstream(std::string(path) + "/test.json").good());
    assert(std::ifstream(std::string(path) + "/test.txt").good());
    // Capacity failures leave existing samples intact and don't expose partial samples.
    {
        Writer writer(path, 95, 1, 100);
        Sample sample; sample.id = "cap_test"; sample.metadata = "{}"; sample.image = image;
        assert(writer.enqueue(sample));
    }
    struct stat info{};
    assert(stat((std::string(path) + "/test.jpg").c_str(), &info) == 0);
    assert(stat((std::string(path) + "/cap_test.jpg").c_str(), &info) != 0);
    // 默认输出只有 JPG：即使提供候选数据，开关关闭时也不生成任何附加文件。
    {
        Writer writer(path, 95, 1024 * 1024, 1);
        Sample sample; sample.id = "image_only"; sample.image = image;
        sample.metadata = "{}"; sample.labels = "x\ny\n"; sample.annotation = "0 0.5 0.5 0.2 0.2\n";
        assert(writer.enqueue(sample));
    }
    assert(std::ifstream(std::string(path) + "/image_only.jpg").good());
    for (const char* extension : {".json", ".txt", ".labels.txt"})
        assert(!std::ifstream(std::string(path) + "/image_only" + extension).good());
    for (const char* file : {"test.jpg", "test.json", "test.txt", "test.labels.txt", "image_only.jpg"}) unlink((std::string(path) + "/" + file).c_str());
    rmdir(path);
    std::cout << "Dataset rule truth tables, validation, timing and writer passed\n";
}
