# YOLOv8-OBB 集成

模型类型为 `yolov8_obb`。已接入模型工厂、配置校验、Web 模型能力列表、张量检查、组合模型、RGA 零拷贝、ROI 坐标映射及显示。使用项目统一 RKNN 2.4.2a2 runtime，无需链接独立 demo 的全局变量或图像工具。

## 使用

Web 编辑器中添加模型节点，选择 `yolov8_obb`，模型为 `assets/yolov8n_obb.rknn`，标签可选 `assets/labels_dota.txt`。没有标签时显示 `class_N`；自训练模型应使用自己的类别顺序，类别数量必须匹配。建议初始置信度 0.5、NMS 0.4，NPU 核心设为自动（`-1`）。与检测、姿态、分割、深度模型按现有方式连接到同一通道或不同通道。

可直接使用 `projects/assets/obb_multi_model.json`，其中同一视频配置 OBB、安全帽检测、姿态三个模型。该示例不替换已有运行配置：

```bash
cd /userdata/rk3588_visual_analysis_framework/projects
../build/engine/vision_analysis assets/obb_multi_model.json
```

示例关闭本地显示和 RTSP，启用性能输出；需要预览时在 Web 中打开相应开关。更新已有发布包时，需重新构建/发布引擎和控制台，旧运行进程不会自动获得新模型类型。

## 多模型与数据契约

- RKNN context、输入 DMA 内存、RGA handle、标签、量化表、候选框及输出缓存均属于各自实例。
- 复用 `ModelBase::infer_mtx`，锁覆盖 RGA 写输入到完成推理；共享模型跨通道不能同时修改输入。
- 复用 `npu::Execution` 核心门控与输出租约：同核/重叠核掩码排队，不重叠核心可并行；异常也释放已获取输出。
- 保留 `model_id`、`model_type`、`model_index` 来源标识；NMS 在模型内部按类别运行，ROI 合并按模型隔离。
- `AlgoResult::has_obb` 表示旋转框，`obb_corners` 为四个顶点，采用与 `box` 相同的业务坐标。ROI 和非等比拉伸分别变换四个顶点，不用一个角度错误表示变形后的四边形。
- `box` 保留四边形的轴对齐包围框，供原有业务判断及跟踪使用；绘图使用真实四角。跟踪匹配仍使用包围框，未改为旋转框跟踪算法。
- 单个 OBB 最多 128 个结果，组合模型不会再先截为 20 个，也扩大了组合结果预算，避免 OBB 挤掉其他模型的结果。

## 性能与算法

复用既有 RGA→RKNN DMA 输入及缓存 handle。INT8 DFL 使用每头 256 项指数表；置信度使用 sigmoid 查表；同一网格多类别命中只解码一次框。候选容器复用容量，NMS 的协方差与行列式只计算一次，得到前 128 个保留框后停止。

OBB 采用三个 `64+类别数` 的 NCHW 检测头（步长 8/16/32）和一个角度输出。INT8 原生输出直接解码，其他输出通过 RKNN 转成 FP32。不兼容布局在加载时拒绝。

解码参考官方示例 `837db8e992815ff8647eadb709f07067f4bc3a71`（来源见 `yolov8_obb_demo/UPSTREAM.md`）。集成版修复官方 NMS 的排序后类别索引、第二个框角度引用问题，并使用矩形高斯协方差 `w²/12、h²/12` 与中心坐标进行 ProbIoU。因此 NMS 结果可能与原始 demo 不完全一致。整帧+ROI 合并使用变换后四边形的几何 IoU，避免包围框误抑制交叉目标。

当前板端测试：预热后 20 次 640×640 零拷贝输入，`infer_ms` 约 14.2 ms，`postprocess_ms`（含取输出和释放）约 7.3–7.5 ms。原独立 demo 的取输出+后处理约 18 ms；此数值是当前机器上的参考，NMS 修正也会影响候选保留数量，并非完全相同算法的微基准。

## 验证

```bash
cmake -S engine -B build/obb-check -DCMAKE_BUILD_TYPE=Release -DBUILD_NPU_TESTS=ON
cmake --build build/obb-check --target obb_runtime_tests -j4
(cd build/obb-check && ctest -R obb_runtime_regression --output-on-failure)
```

硬件测试创建两个标签不同的 OBB 实例，以及检测、姿态实例；同核/跨核两种模式共 240 次并发推理，逐次对比独立基线的数量、类别、标签、置信度、包围框及四角。另验证 40 次真实 RGA 写 DMA 的输出一致性和非等比 ROI 坐标映射。测试需要当前 NPU 设备可由测试进程独占。

源码关键位置：`engine/src/yolo/yolov8det.cpp` 复用执行生命周期；`yolov8obb_impl.h` 为独立 OBB 解码；`engine/tests/inference/test_obb_runtime.cpp` 为板端回归。后端张量兼容测试覆盖错误输出数量、类别数、布局及角度数量。

完整引擎还完成了 60 秒同核混合视频测试（OBB + 1088 安全帽检测 + 姿态，三个模型共用核心 0），每个统计窗口 `failed=0`，组合推理约 11.9 FPS；RTSP 客户端解码 1781 帧，停止后 0.52 秒正常退出。临时报告及日志已按要求清理，验证结论保留在本文。23 项现有 CTest 回归、31 项后端模型检查测试以及前端生产构建通过。
