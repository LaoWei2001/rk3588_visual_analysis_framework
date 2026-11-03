# ZipDepth 引擎适配

`ZipDepth : ModelBase` 对接本项目 `zipdepth/zipdepth-rknn` 的 RKNN 导出契约，
注册类型为 `zip_depth`。运行时使用引擎锁定的 RKNN 2.4.2a2 和现有 RGA3 管线，
无需另带原项目的动态加载器、RKNN 头文件、Python 或图片保存程序。

## 兼容性与改进

| 原程序 | 引擎适配 |
| --- | --- |
| 独立图片推理、动态加载 RKNN | ModelBase、统一模型工厂、热重载、多模型同帧分发 |
| RGB uint8 NHWC，固定尺寸缩放 | 保留 RGB、线性缩放及 pass_through=0；不重复做 /255 或 ImageNet 归一化 |
| 输出相对逆深度，越大越近 | AlgoResult::depth_kind=RelativeInverse，禁止标成米制距离 |
| 每次分配/复制输出 | 预分配 float32 输出，最多缓存 4 个槽，仅复用无外部读者的槽 |
| 原图尺寸逐像素恢复 | 保留模型原生输出，直接一次映射到业务坐标，使用 align_corners=True 插值 |
| 普通 inputs_set 输入 | 优先 RGA 写 RKNN DMA 输入；初始化时缓存 RGA handle；CPU 路径复用 resize/RGB 缓冲 |

输入和输出宽高从 RKNN 张量读取，不根据文件名或 JSON 猜测。要求 batch=1、单 RGB 输入、
单通道深度输出；支持 NCHW/NHWC 输入、2/3/4 维深度平面、FP32/FP16/INT8/UINT8 输出。
输出可比输入低分辨率，但必须保持宽高比。非对齐或带额外输入 stride 的模型走 inputs_set
回退，避免 RGA 写错布局。量化模型仍需由模型提供方验证精度和归一化契约。

相对深度有效值允许 0 和负数，ROI 外部使用 NaN；SDK 使用位检查过滤非有限值，
兼容引擎的 `-ffast-math`。预览直接使用相对逆深度，显示红近蓝远，不再次取倒数。

## 尺寸与调度

- 384×384、672×384、768×768（这里按 W×H）以及其他符合契约的固定尺寸无需改代码。
  切换 `model_path` 即切换模型输入分辨率，不需要修改视频尺寸或业务坐标。
- 当前仓库提供的模型只有 **768×768 FP16**；不能通过配置宽高把它变成 384 模型。
  使用其他尺寸需要对应尺寸已导出的 RKNN 文件。
- 每个模型直接从视频源缩放到自身输入，最终统一映射到 640×640，支持与不同尺寸 YOLO 同帧组合。
  更大输入保留更多推理细节，但 SDK 深度图仍为 640×640；不保证更大模型在所有场景更准。
- 支持 `roi_only` 的 stretch/letterbox/expand 以及 `full_frame_roi_filter`。
  `full_plus_roi` 对 ZipDepth 只运行整帧，对其他模型仍按原配置执行。
  原因是不同裁剪的相对深度尺度/偏移不同，直接覆盖合并没有可比意义。
- 多个 worker 独立持有上下文；引擎按通道、worker、子模型序号为自动模式分配明确的 0/1/2 核心，显式指定核心时尊重配置。
  同一进程中所有 RKNN 模型（包括 YOLO26 Depth、检测、姿态、分割和 ZipDepth）共用核心保护，重叠核心串行，不同核心并行。
  直接调用模型而传入 runtime AUTO、或设置核心失败时，实际核心未知，保守占用全部核心。
  详见 [NPU 调度](../../inference/NPU_SCHEDULING.md)。
  默认示例用 3 个 worker；内存紧张或与检测模型组合时可改为 1 个，需要权衡吞吐、延迟和内存。

## 验证

```bash
cmake -S engine -B build/engine -DBUILD_ZIP_DEPTH_TESTS=ON -DBUILD_DEPTH_TESTS=ON -DBUILD_INFERENCE_ROI_TESTS=ON
cmake --build build/engine -j4
cd build/engine
ctest --output-on-failure -R '^(zip_depth_runtime|depth_result|business_coordinates|model_status|frame_snapshot)_regression$'
```

硬件测试比较独立 inputs_set 参考与 DMA 推理，实际执行 RGA→RKNN 输入传输，
检查跨帧持有、非连续图像和非法输入，并分别报告 CPU 输入/DMA 就绪输入的耗时。
数值与几何测试覆盖三种尺寸、负数/零/NaN、ROI、预览方向与混合模型。
只有本机提供的 768×768 模型执行硬件推理；其他尺寸执行几何回归。

业务使用方法与示例配置见 [logic_zip_depth_demo](../../../../projects/modules/logic_zip_depth_demo/README.md)。

## 本机验证记录（2026-10-26）

RK3588，RKNN runtime 2.4.2a2，驱动 0.9.0，Release 构建，768×768 FP16。
独立 `inputs_set` 与优化后的 DMA / RGA→DMA 输出最大绝对误差均为 **0**。

| 场景 | 实测 |
| --- | --- |
| DMA 输入已就绪，含 640×640 映射（12 帧均值） | 183.2 ms/帧，映射约 2.0 ms |
| BGR 图片输入，含预处理与映射（12 帧均值） | 187.8 ms/帧 |
| MP4 视频，1 worker，关闭显示/RTSP | 4.08–4.46 次推理/秒 |
| 同视频，3 worker，核心 0/1/2 | 11.40–12.34 次推理/秒；实际发布 9.80–11.48 帧/秒 |
| 同帧 1088 YOLO + 768 ZipDepth，独立核心 | 3.91–4.06 批/秒，两个模型均成功 |

视频为 `projects/assets/10_11_1.mp4`，使用硬件 MPP 解码、queue_size=1，三个视频测试
分别运行约 20/18/18 秒，失败计数均为 0。单帧测试不含解码、RGA 准备、编码或网络发送，
不能将其倒数直接当作完整视频 FPS。多 worker 会按既有规则丢弃过时结果，所以发布帧率
可低于推理吞吐；以上数值不是实时预览性能保证。未改写模型提供方的原始验证元数据。

额外注入一次 `pthread_create` 失败，确认 1088 YOLO + 768 ZipDepth 自动串行回退后仍独立取图、正常发布，连续视频测试失败计数为 0。

RTSP 实测通过本地 TCP 客户端读取硬件 H.264 编码的 1280×720 深度预览，跳过启动黑帧后确认热力图及相对深度文字正常；测试进程均正常退出。

## 同核并发保护（2026-10-26）

现场配置为 `channel_threads=3`、`npu_core=0` 时，三个独立 ZipDepth 上下文同时使用核心 0，
驱动出现 `job abort ret=-22`，并有线程停留在 `rknpu_submit_ioctl`、无法正常退出。
此前测试覆盖的是自动分配至三个不同核心，未覆盖这一组合。

首次修复仅在 ZipDepth 内对重叠核心持有互斥锁，覆盖 run/get/release 整个周期。
该版本未覆盖其他模型类型，随后在双路 ZipDepth + YOLO26 Depth 中再次出现同核提交超时。
现已改用所有 RKNN 模型共享的 `inference/npu_execution.h` 和 `npu_core_gate.h`，
不再保留独立的 ZipDepth 锁。提交、取结果或释放失败后，整个进程停止后续 NPU 提交，
因为驱动恢复可能重置整块设备。5000 ms 同步超时不能保证取消已经卡在内核中的调用；
仍需设备恢复后重启推理进程。引擎和本项目的硬件测试/基准程序另有进程级 NPU 占用锁。

`npu_core_gate_regression` 检查核心分配、同核互斥、异核并行、AUTO 重叠和异常释放。
`npu_execution_*` 检查不同编译单元共享同一把锁，以及 run/get/release 失败后的停止提交。

修复后以现场 `depth_multi_model.json`（3 worker、768×768 FP16、视频循环、1280×720 RTSP）
各运行 120 秒，并由本地 TCP 客户端持续解码 RTSP；测试仅更换 RTSP 端口以免冲突。
固定核心 0：3.78–4.21 次推理/秒；自动分配核心 0/1/2：约 10.8–11.9 次推理/秒。
两轮推理失败均为 0，停止分别耗时 1.72 / 1.12 秒，退出码均为 0。
这是本机两分钟回归结果，不代表所有模型、驱动或长时间负载的稳定性保证。
