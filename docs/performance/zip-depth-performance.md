# ZipDepth 视频性能排查（2026-10-27）

历史实测：2026-10-27。算子瓶颈与数值仅对应文中被测权重，不能外推所有 ZipDepth 导出；复现命令已按当前 CMake 选项核对。

本次被测的 768×768 FP16 RKNN 包含 CPU 输入转换和 CPU Transpose，不能按“全部算子都在 NPU 上执行”估算速度。
单路三 worker 已分别使用 NPU 0/1/2，不是漏开三核；脱离解码、RGA、业务、显示和网络的裸模型测试
也只有约 12 FPS，各核利用率约 60%～70%。

## 瓶颈证据

`build/zip-depth-investigation/operators.log` 的一次逐层报告：

| 算子 | 执行设备 | 报告耗时 |
| --- | --- | ---: |
| InputOperator:image | CPU | 64.387 ms |
| Transpose:/encoder/stage2/stage2.2/Add_1_output_0_rs | CPU | 34.088 ms |
| OutputOperator:depth | CPU | 0.826 ms |
| CPU 合计 | CPU | 99.301 ms |
| NPU 合计 | NPU | 142.024 ms |

**这是开启逐层性能采集后的单次数据，不是生产模式每帧的准确耗时，也不能据此预言 64 ms 的生产加速。**
采集开关会降低帧率，见 [锁定 RKNN API 的开关说明](../../engine/vendor/rknn/2.4.2a2/include/rknn_api.h)。
实际收益必须看下面关闭采集的完整视频对照。

输入绑定 DMA 并不表示免除类型转换：RGA 写入的是 RGB8，模型原生输入是 NHWC FP16；
原路径 `pass_through=0` 仍让 RKNN 完成输入转换和 `/255`。
网络内部的 CPU Transpose 会中断该次推理的 NPU 计算，是剩余利用率空隙的一个已确认来源。
FP16 网络与检测网络的算子、精度和中间张量规模也不同，不能通过比较 NPU 占用率直接判断调度是否正确。

## 改动

`engine/src/vision/models/zip-depth/zip_depth.cpp` 保留 RGA 的 RGB8 目标缓冲，另绑定模型原生 FP16 输入。
用 ARM NEON 批量完成 RGB8 → FP16 `/255`，再以 `pass_through=1` 提交，ImageNet 归一化仍由原网络执行。
CPU 转换计入 `preprocess_ms`，在取得 NPU 核心前执行；全部 RKNN 执行和输出读取仍受原核心保护。
未改变权重、输入尺寸、输出精度、核心规划、队列或丢弃过期结果的规则。

只有原生 NHWC FP16、紧凑 stride 的输入才尝试此路径。每个实例加载时，用覆盖全部 256 个字节值、
不同通道和空间位置的探针分别经过原转换和新转换，要求浮点深度输出逐值一致且有限。
格式不支持、内存申请失败或探针对照不一致时继续使用原转换。
这是针对标准 ZipDepth `/255` 导出契约的优化和加载检查，不是任意网络归一化方式的推断接口。

额外成本：每个 768×768 worker 增加约 3.4 MiB 的 FP16 输入缓冲，加载时执行两次校验推理。
校验只在模型创建/重新加载时执行，不计入视频的稳态测量窗口。

## 实机对照方法

RK3588，锁定 Runtime 2.4.2a2、现有 driver 0.9.0；同一 `10_11_1.mp4` 循环播放，
768×768 原始 FP16 权重，max_fps=25、queue_size=1、1280×720 H.264 RTSP 输出，FFmpeg 持续解码。
不调整 CPU/NPU/DDR 频率、governor 或 CPU 亲和性。
预热 12 秒，继续运行 35 秒，排除跨预热边界的日志窗口；有效窗口约 30 秒。
三 worker 按旧→新→新→旧运行；一 worker 和混合负载各运行旧→新一次。
按窗口时间加权计算完成/发布 FPS，按成功帧数加权计算 queue+worker 耗时。

原始配置、每窗口日志、频率/温度和 RTSP 解码记录当时写入 `build/zip-depth-investigation/`，临时目录未随 Git 发布且不保证仍存在。
发布 FPS 是结果槽更新次数，RTSP 是独立的显示节拍，二者不能混用。
CPU 百分比的 100% 表示一个 CPU 核心；NPU 利用率是离散采样，不能用一次瞬时 80% 代替全程均值。
queue+worker 不包含摄像头到播放器的完整延迟。

| 场景 | 完成 FPS，旧→新 | 发布 FPS，旧→新 | queue+worker ms，旧→新 |
| --- | ---: | ---: | ---: |
| 单路三 worker，三核 | 11.403 → 11.653 | 10.468 → 10.682 | 272.625 → 268.255 |
| 单路一 worker，单核 | 4.375 → 4.557 | 4.375 → 4.557 | 250.883 → 241.044 |
| 混合：ZipDepth 固定核 0 | 3.870 → 3.978 | 3.870 → 3.978 | 795.413 → 779.697 |
| 混合：YOLOv5n 固定核 1 | 24.989 → 24.991 | 24.989 → 24.991 | 17.515 → 17.595 |

三 worker 完成/发布分别约 +2.19% / +2.04%，一 worker 约 +4.17%，混合负载的 ZipDepth 约 +2.77%。
**收益小，不能用单实例或逐层采集的时间差宣传成视频大幅提速。** 单轮数据也可能包含运行波动。
混合负载的 YOLO 吞吐持平，queue+worker 小幅上升约 0.08 ms（0.45%），不能宣称所有指标严格无回退。
混合场景两路各三个 worker、固定单核，故 ZipDepth 的等待比单 worker 更长；此次保留配置做同条件对照。
检测测试使用同一份现有标签，不包含类别语义准确率验证。

三 worker 的逐核 NPU 采样均值为旧版 62.25% / 61.75% / 63.75%，
新版 64.25% / 65.88% / 65.38%，并未持续达到 80%。引擎 CPU 均值 224.19% → 218.82%。
一 worker 的工作核心为 57.5% → 62.0%。这些持续采样修正了调试期间单次读取 80% 的印象。
三核收益收窄的进一步原因可能包括内存带宽和调度竞争，本次没有带宽计数证据，不把它们写成已证实结论。
八次有效管线运行均持续推理、发布和解码 RTSP，零推理失败，并正常停止。

## 验证和复现

原图与独立 `rknn_inputs_set` 参考路径的最大深度误差为 0；真实 RGA → RGB DMA → FP16 路径误差为 0；
另一个覆盖 RGB 全字节范围的彩色梯度误差也为 0。保留旧帧深度图、非连续输入、无效输入、
640×640 业务坐标映射继续通过。三个同核实例共完成 24 次普通输入和 24 次 DMA 输入，无失败。
调度、异常、核心组合、跨进程占用、结果映射等 13 项回归通过。

硬件回归在有权重时加入 CTest；没有示例图片则使用固定随机种子的合成图片，避免静默跳过：

```bash
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release -DBUILD_ZIP_DEPTH_TESTS=ON
cmake --build build/engine -j4 --target vision_analysis zip_depth_runtime_tests
cd build/engine
ctest --output-on-failure -R zip_depth_runtime
```

独立[逐层诊断工具](../../tools/device/rknn_profile/README.md)只需要锁定的 SDK，从仓库根目录构建和运行，执行前应让其他 NPU 推理程序退出：

```bash
./tools/device/rknn_profile/build.sh
./tools/device/rknn_profile/rknn_profile projects/assets/zipdepth_npu_768x768-rk3588-fp16.rknn 5
```

原权重仍包含 CPU Transpose，本次没有消除它。进一步优化需要原始 ONNX/导出工程，检查该
Transpose 的布局和邻接算子后重新转换，并比较数值和实际视频吞吐。
本次排查所用素材只有已编译 RKNN，不能声称仅靠增加线程就能把这些 CPU 算子搬到 NPU。
