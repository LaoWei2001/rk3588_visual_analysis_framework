# `src/yolo`：RKNN 模型实现

## 当前实现

| 配置 `model_type` | 类/文件 | 主要输出 |
|---|---|---|
| `yolov5` | `YOLO` / `yolo.*` | 检测框 |
| `yolov8_det` | `YoloV8Det` / `yolov8det.*` | 检测框 |
| `yolov8_pose` | `YoloPose` / `yolopose.*` | 检测框、关键点和分数 |
| `yolov5_seg` | `YoloSeg` / `yoloseg.*` | 检测框和 mask |

`model_base.h` 定义统一接口；`composite_model.*` 把同通道多个子模型的结果过滤、标注来源并合并。模型创建工厂实际位于 `src/analyzer/algo_engine.cpp`，不是 yolo 目录。

## `ModelBase` 契约

派生类必须实现 `infer()`、输入宽高、阈值设置/读取；支持 DMA-BUF 零拷贝时覆盖 `get_input_fd()`、`get_input_rga_handle()` 和 `infer_zero_copy()`。`nms_done()` 告诉上层后处理是否已经做过 NMS。

基类的 `infer_mtx` 保护共享 RKNN context；锁范围需要覆盖 RGA 写模型输入缓冲到 NPU 推理完成。不要只锁 `infer_zero_copy()` 而让另一个通道同时覆盖输入。

## 单模型与多模型

单模型配置经 `create_model(type, model_path, label_path, core_mask, obj, nms)` 生成 `shared_ptr<ModelBase>`。NPU core 允许 0、1、2；未指定时按实例轮转分配。

当 `ChannelConfig.models[]` 有多个有效条目时，推理层为每个条目创建子模型并包装为 `CompositeModel`。持久并行执行器让子模型在同一帧上运行，再由 `merge_child_results()`：

- 按每个子模型自己的类别集合过滤；
- 填 `model_id`、`model_type`、`model_index`；
- 合并性能统计和结果；
- 保持上层仍接收统一 `vector<AlgoResult>`。

因此通道级外层类别过滤不能再次错误过滤使用不同标签表的子模型。

## 输入与坐标

analyzer 把源帧整幅 resize 到模型声明的输入宽高，不做 letterbox；模型后处理、ROI、logic 和 render 共享这一坐标系。新增模型若自身需要 letterbox，必须同时提供反变换，并评估整个项目的统一坐标约定，不能只在后处理局部改框。

## 接入新模型类型

1. 新建继承 `ModelBase` 的头/源文件，正确管理 RKNN context、tensor/buffer 和 RGA handle 生命周期。
2. 把输出转换为 `AlgoResult`；模型专有数据使用已有扩展字段，确有需要再扩展公共结构及所有复制/渲染方。
3. 在 `algo_engine.cpp::create_model()` 增加类型分支。
4. 在 `config_validator.cpp` 支持该类型，并把同名字符串加入 `src/logic/catalog.json` 的 `model_types` 数组；正常打包会聚合到 App `logics.json`，同时确认 Web 模型节点选项链路。
5. 检查单模型、`CompositeModel`、热 reload、阈值/类别更新和 NPU core 分配。

tracker 不属于 yolo 模块，它在 analyzer 的 channel pipeline 中对合并后的结果运行。不要在每个模型派生类内各自分配 track id。
