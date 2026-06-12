# YOLO 推理模块 (yolo/)

> 基于 RKNN Runtime 的 NPU 推理引擎，提供统一的模型抽象基类和多种 YOLO 变体实现。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `model_base.h` | `ModelBase` 抽象基类，定义推理接口 |
| `yolo_utils.h` | 公用工具：LetterBox、NMS、坐标变换等 |
| `yolo.h / yolo.cpp` | YOLOv5 检测（通用 RKNN 推理实现） |
| `yolov8det.h / yolov8det.cpp` | YOLOv8 检测（后处理与 v5 有差异） |
| `yolopose.h / yolopose.cpp` | YOLOv8-Pose 关键点检测 |
| `yoloseg.h / yoloseg.cpp` | YOLOv5-Seg 实例分割 |

---

## 架构设计

### 模型基类（ModelBase）

所有模型类型继承自 `ModelBase`，推理管线（`algoProcess.cpp`）只依赖此接口，做到推理引擎与业务逻辑解耦：

```cpp
class ModelBase {
public:
    // 标准推理（CPU 前处理 + NPU 推理 + CPU 后处理）
    virtual bool infer(cv::Mat &frame, vector<AlgoResult> &results,
                       YoloPerfStat *perf = nullptr) = 0;

    // 零拷贝推理（RGA 直接写入 RKNN 输入内存，跳过 memcpy）
    virtual bool infer_zero_copy(vector<AlgoResult> &results,
                                 YoloPerfStat *perf = nullptr) { return false; }

    // 获取 RKNN 输入内存的 DMA-BUF 句柄（供 RGA 零拷贝使用）
    virtual int get_input_fd() const { return -1; }

    virtual int   input_width() const = 0;
    virtual int   input_height() const = 0;
    virtual void  set_thresh(float obj_thresh, float nms_thresh) = 0;
    virtual float get_obj_thresh() const = 0;

    // 若模型后处理已内置 NMS，返回 true，管线跳过外部 NMS
    virtual bool  nms_done() const { return false; }
};
```

### 推理流程

**标准路径（`infer`）：**
```
cv::Mat (BGR, 640×640)
    ↓ preprocess()  letterbox 缩放 + BGR→RGB + 归一化
    ↓ rknn_inputs_set()
    ↓ rknn_run()    NPU 执行
    ↓ rknn_outputs_get()
    ↓ post_process()  解码特征图 → 置信度过滤 → NMS
    ↓ vector<AlgoResult>
```

**零拷贝路径（`infer_zero_copy`）：**
```
RGA 直接写入 in_mem_->fd 所指向的物理内存（NV12 → RGB888）
    ↓ rknn_run()    NPU 执行（无需 CPU 参与前处理）
    ↓ post_process()
    ↓ vector<AlgoResult>
```

零拷贝路径可节省一次 `memcpy`（约 640×640×3 ≈ 1.2MB），在多路场景下效果显著。

### AlgoResult — 推理结果

```cpp
struct AlgoResult {
    cv::Rect  box;            // 检测框（模型输入坐标系，如 640×640）
    string    label;          // 类别名称
    int       class_id;       // 类别索引
    float     score;          // 置信度
    int       track_id;       // 目标跟踪 ID（-1=未跟踪）
    int       chn_id;         // 来源通道号
    int64_t   frame_id;       // 产出本结果的帧序号
    uint64_t  timestamp_ms;   // 产出本结果的时间戳（毫秒）
    cv::Scalar box_color;     // 自定义显示颜色（(-1,-1,-1)=使用默认色）

    // 便捷方法
    cv::Point box_center() const;          // 框中心点
    bool      box_contains(cv::Point) const; // 点是否在框内
    int       dist_sq_to(cv::Point) const; // 中心点到指定点的距离平方

    // 模型特有字段
    vector<cv::Point2f> keypoints;  // 关键点（Pose 模型）
    cv::Mat             boxMask;    // 分割 mask（Seg 模型）
};
```

---

## 支持的模型类型

| 配置值（`model_type`） | 对应类 | 说明 |
|------------------------|--------|------|
| `"yolov5"` | `YOLO` | YOLOv5 目标检测 |
| `"yolov8_det"` | `YOLOv8Det` | YOLOv8 目标检测 |
| `"yolov8_pose"` | `YOLOPose` | YOLOv8-Pose 关键点检测 |
| `"yolov5_seg"` | `YOLOSeg` | YOLOv5 实例分割 |

---

## 性能统计（YoloPerfStat）

每次推理可选填充性能统计结构：

```cpp
struct YoloPerfStat {
    float preprocess_ms  = 0.0f;  // 前处理耗时
    float infer_ms       = 0.0f;  // NPU 推理耗时
    float postprocess_ms = 0.0f;  // 后处理耗时
};
```

性能数据汇总后在终端以 Feed 统计日志形式输出（`performance_display=true` 时）。

---

## 二次开发指南

### 接入新模型类型

1. 新建头文件和实现文件，继承 `ModelBase`：

```cpp
// src/yolo/mymodel.h
#include "model_base.h"

class MyModel : public ModelBase {
public:
    MyModel(const string &model_path, const string &label_path,
            int core_mask, float obj_thresh, float nms_thresh);

    bool infer(cv::Mat &frame, vector<AlgoResult> &results,
               YoloPerfStat *perf = nullptr) override;

    int  input_width()  const override { return model_w_; }
    int  input_height() const override { return model_h_; }
    void set_thresh(float obj, float nms) override { obj_thresh_=obj; nms_thresh_=nms; }
    float get_obj_thresh() const override { return obj_thresh_; }

private:
    // ... RKNN 上下文、标签等成员 ...
};
```

2. 在 `algoProcess.cpp` 的模型工厂函数 `create_model()` 中添加新类型的分支：

```cpp
// algoProcess.cpp 的 create_model 函数中
if (cfg.model_type == "my_model") {
    return make_unique<MyModel>(cfg.model_path, cfg.label_path,
                                core_mask, cfg.obj_thresh, cfg.nms_thresh);
}
```

3. 在 `config_validator.cpp` 中将 `"my_model"` 加入合法的 `model_type` 列表。

### 自定义后处理

覆盖 `infer()` 方法，在调用 `rknn_run()` 后实现自己的特征图解码：

```cpp
bool MyModel::infer(cv::Mat &frame, vector<AlgoResult> &results, YoloPerfStat *perf) {
    // 1. 前处理（letterbox + 归一化）
    auto lb = preprocess(frame);

    // 2. NPU 推理
    rknn_run(ctx_, nullptr);

    // 3. 自定义后处理
    rknn_output outputs[num_out];
    rknn_outputs_get(ctx_, num_out, outputs, nullptr);
    my_decode(outputs, lb, results);
    rknn_outputs_release(ctx_, num_out, outputs);
    return true;
}
```

### 修改置信度阈值

阈值可在运行时热更新，无需重启：

```cpp
// 通过 algoProcess 接口更新（会转调 model->set_thresh）
algorithm_update_thresh(chnId, new_obj_thresh, new_nms_thresh);

// 或者直接在 config.json 中修改并触发热重载
```

### 使用跟踪 ID

`AlgoResult::track_id` 由 `Tracker`（SORT 算法）在 `analyzer.cpp` 中填入，`yolo` 模块本身不负责跟踪。业务逻辑通过 `r.track_id` 访问：

```cpp
for (auto &r : ctx->results) {
    if (r.track_id > 0) {
        // r.track_id 是稳定的跨帧轨迹 ID，可用于目标计数、轨迹绘制等
    }
}
```
