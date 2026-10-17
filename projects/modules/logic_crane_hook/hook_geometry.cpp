#include "hook_geometry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace crane_safety
{
namespace
{
std::vector<std::string> csv_row(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ','))
    {
        if (!field.empty() && field.back() == '\r') field.pop_back();
        fields.push_back(field);
    }
    return fields;
}

bool number(const std::string &text, double *out)
{
    // Reject NaN/Inf lexically as the engine also builds with -ffast-math.
    if (text.empty() || text.find_first_not_of("0123456789.eE+-") != std::string::npos)
        return false;
    try
    {
        size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size() || value < -100000.0 || value > 100000.0) return false;
        *out = value;
        return true;
    }
    catch (...) { return false; }
}
} // namespace

HookEdges hook_edges(const cv::Rect &box, const cv::Size &frame, int margin)
{
    HookEdges out;
    if (box.width <= 0 || box.height <= 0 || frame.width <= 0 || frame.height <= 0)
        return out;
    const double right = static_cast<double>(box.x) + box.width;
    const double bottom = static_cast<double>(box.y) + box.height;
    out.valid = right > 0 && bottom > 0 && box.x < frame.width && box.y < frame.height;
    const int edge = std::max(0, margin);
    out.left = box.x <= edge;
    out.right = right >= frame.width - edge;
    out.top = box.y <= edge;
    out.bottom = bottom >= frame.height - edge;
    return out;
}

HookCalibration load_hook_calibration(const std::string &path, double height_bin,
                                     const cv::Size &frame, int margin)
{
    HookCalibration result;
    std::ifstream input(path);
    std::string line;
    if (!input.is_open() || !std::getline(input, line))
    {
        result.error = "无法读取标定文件: " + path;
        return result;
    }
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    const auto header = csv_row(line);
    auto column = [&](const char *name) {
        auto it = std::find(header.begin(), header.end(), name);
        return it == header.end() ? -1 : static_cast<int>(it - header.begin());
    };
    const std::vector<int> columns = {column("box_x"), column("box_y"), column("box_width"),
                                      column("box_height"), column("center_x"), column("center_y")};
    if (*std::min_element(columns.begin(), columns.end()) < 0)
    {
        result.error = "标定CSV缺少完整框坐标、宽高或中心列，请重新采集";
        return result;
    }
    const int frame_w = column("frame_width"), frame_h = column("frame_height");
    if ((frame_w >= 0) != (frame_h >= 0))
    {
        result.error = "标定CSV画面尺寸列不完整";
        return result;
    }
    const int last_column = std::max(*std::max_element(columns.begin(), columns.end()), std::max(frame_w, frame_h));
    std::vector<HookCalibrationPoint> raw;
    for (int row = 0; row < 100000 && std::getline(input, line); ++row)
    {
        if (line.empty() || line == "\r") continue;
        ++result.rejected;
        const auto fields = csv_row(line);
        if (static_cast<int>(fields.size()) <= last_column) continue;
        double values[6];
        bool valid = true;
        for (int i = 0; i < 6; ++i)
            valid = number(fields[columns[i]], &values[i]) && valid;
        if (!valid) continue;
        // Old CSVs use the fixed 640x640 business canvas, not native video pixels.
        double width = 640, height = 640;
        if (frame_w >= 0 && (!number(fields[frame_w], &width) || !number(fields[frame_h], &height))) continue;
        if (width != frame.width || height != frame.height) continue;
        const cv::Rect box(static_cast<int>(std::lround(values[0])), static_cast<int>(std::lround(values[1])),
                           static_cast<int>(std::lround(values[2])), static_cast<int>(std::lround(values[3])));
        if (!hook_edges(box, frame, margin).complete()) continue;
        if (std::abs(values[4] - (box.x + box.width / 2)) > 1.0 ||
            std::abs(values[5] - (box.y + box.height / 2)) > 1.0) continue;
        HookCalibrationPoint point;
        point.width = box.width; point.height = box.height;
        point.center_x = values[4]; point.center_y = values[5];
        raw.push_back(point);
        --result.rejected;
        ++result.accepted;
    }
    if (raw.empty())
    {
        result.error = "没有完整可见且尺寸匹配的标定样本，请重新采集";
        return result;
    }
    std::sort(raw.begin(), raw.end(), [](const HookCalibrationPoint &a, const HookCalibrationPoint &b) {
        return a.height < b.height;
    });
    const double bin = std::max(.1, height_bin);
    // Permit only half a bin of detection jitter outside the sampled full heights.
    result.min_height = std::max(1.0, raw.front().height - bin / 2);
    result.max_height = raw.back().height + bin / 2;
    long long previous_bin = -1;
    int count = 0;
    for (const auto &sample : raw)
    {
        const long long key = std::llround(sample.height / bin);
        if (result.points.empty() || key != previous_bin)
        {
            result.points.push_back(sample);
            previous_bin = key;
            count = 1;
        }
        else
        {
            auto &point = result.points.back();
            point.width += (sample.width - point.width) / (count + 1);
            point.height += (sample.height - point.height) / (count + 1);
            point.center_x += (sample.center_x - point.center_x) / (count + 1);
            point.center_y += (sample.center_y - point.center_y) / (count + 1);
            ++count;
        }
    }
    return result;
}

bool HookCalibration::lookup(double height, HookCalibrationPoint *point) const
{
    if (!point || points.empty() || height < min_height || height > max_height) return false;
    auto right = std::upper_bound(points.begin(), points.end(), height,
        [](double value, const HookCalibrationPoint &p) { return value < p.height; });
    if (right == points.begin()) { *point = points.front(); return true; }
    if (right == points.end()) { *point = points.back(); return true; }
    const auto &a = *(right - 1), &b = *right;
    const double fraction = (height - a.height) / (b.height - a.height);
    point->height = height;
    point->width = a.width + (b.width - a.width) * fraction;
    point->center_x = a.center_x + (b.center_x - a.center_x) * fraction;
    point->center_y = a.center_y + (b.center_y - a.center_y) * fraction;
    return true;
}

HookGeometry measure_hook(const cv::Rect &box, const cv::Size &frame, int margin,
                          const HookCalibration &calibration, bool require_calibration)
{
    HookGeometry out;
    const HookEdges edges = hook_edges(box, frame, margin);
    out.truncated = edges.truncated();
    if (!edges.valid) { out.status = "检测框无效"; return out; }
    if (edges.truncated())
    {
        // Rotation makes width unsuitable for reconstructing a cropped height.
        // Never use a cropped box center or guess its missing extent.
        out.status = "贴边或出画，位置无法可靠判断";
        return out;
    }
    HookCalibrationPoint point;
    const bool matched = calibration.lookup(box.height, &point);
    if (require_calibration && !matched)
    {
        out.status = calibration.points.empty() ? "动态标定不可用，请检查标定文件"
                                               : "完整框高超出标定范围";
        return out;
    }
    out.center = {box.x + box.width / 2, box.y + box.height / 2};
    out.valid = true;
    out.status = "完整可见，按框高标定";
    out.calibrated = matched;
    if (matched)
        out.safe_center = {static_cast<int>(std::lround(point.center_x)), static_cast<int>(std::lround(point.center_y))};
    return out;
}
} // namespace crane_safety
