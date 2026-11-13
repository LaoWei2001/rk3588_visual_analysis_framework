# RK3588 性能记录与复现

本文汇总历史测量与当前可用的测试入口。历史数字仅对应记录中的二进制、输入和硬件条件；本次文档核对没有重新运行性能负载。

2026-11-02 至 11-03 的真实 RTSP、15 路推理、原生输出优化和模型矩阵见[实时多路推理实测](npu-live-performance.md)。
本页保留 2026-10-26 较早的文件输入实测与测试入口；核心分配规则及 RTSP + USB 对照见[自动核心实测](npu-core-placement.md)。不同输入、输出与版本条件的记录不能直接合并。

[返回首页](../../README.md#性能与验证) · [机器可读记录](data/2026-10-26-baseline.json) · [NPU 调度说明](../../engine/src/vision/inference/NPU_SCHEDULING.md)

## 证据与版本

- 本页历史文件输入基线的引擎 strip 后 SHA-256 为 `95ed15fb5533bd16e5b1923cc9bca02d96737f30f4975b88cc66e33557d0e753`，是全局核心规划之前的版本。它标识测试二进制，不代表未来每次构建的成绩。
- [JSON 摘录](data/2026-10-26-baseline.json)包含 3 种场景、6 次基线运行的汇总、逐窗口计数、CPU、观测频率/温度和正常停止耗时。摘录由本机原始报告及日志导出，每段选中窗口的时间、completed、published 总数与原报告一致。
- 原始产物当时写入本机 `build/deploy-backups/npu-performance-20261026-192644/`，该目录未随 Git 发布且不保证仍存在；摘录不附带完整日志、权重与输入视频。模型 SHA-256 和完整散热记录也未归档，**不能声称第三方可精确复现原数值**，目前可复现的是测试方法。
- 两轮候选优化因某路发布率回退被撤回。[实验报告](../../engine/src/vision/inference/NPU_PERFORMANCE.md)中的“新”列不属于当前保留版本，不能作为本项目加速宣传。

## 已有测试的条件

| 项目 | 记录或复核依据 |
| --- | --- |
| 日期、平台 | 2026-10-26，本机 RK3588 / AArch64 |
| OS / 内核 | Debian 11 / Rockchip BSP `5.10.160`；与当时只读检查一致 |
| Runtime / 驱动 | 仓库固定 RKNN `2.4.2a2`，RKNPU driver `0.9.0` |
| RAM / 板卡身份 | 当时只读探测 MemTotal `8,110,208 kB`，设备树 `RK3588 EVB7 LP4 V10`；原测试未独立记录资产编号 |
| 输入 | 每个通道循环读取 `10_11_1.mp4`；当时 ffprobe 确认本机该文件为 H.264、1280×720、30 FPS；原测试未记录视频哈希 |
| 解码 | 文件使用各自播放/解码管线，优先 MPP；代码不合并相同路径的文件解码器 |
| 模型 | 见下表；精度信息只写已归档部分，文件名不能代替张量/量化清单 |
| 调度 | 每通道 3 worker，`queue_size=1`，`max_fps=25`，`npu_core=-1`，按核心分配与互斥规则运行 |
| 输出负载 | 关闭 HDMI；H.264 RTSP、1280×720，TCP FFmpeg 客户端持续解码；测试端口 18554；两路/六路/组合分别为 1×2 / 2×3 / 1×1 网格 |
| 时长 | 每次预热 15 秒、测量 60 秒，每种场景旧→新→新→旧；本页取其中两次“旧/基线” |
| 统计 | 排除跨预热边界窗口；按窗口实际秒数计算 FPS，按成功任务数加权耗时 |
| 频率 / 温度 | runner 未设置 CPU/NPU 频率；基线采样的 NPU 当前频率为 950 MHz，温度观测在 JSON 中；不代表全程锁频 |
| 散热 | 未完整记录风扇、散热器与环境温度，待补 |

六路与组合场景使用以下六份权重。路径中的文件需要测试者自行提供；原报告没有完整权重哈希和检测模型逐张量量化清单：

| 文件 | 类型 / 输入 | 已记录精度 |
| --- | --- | --- |
| `yolov5s_relu.rknn` | `yolov5`，640×640 | 未完整归档 |
| `diaogou_s_107.rknn` | `yolov8_det`，640×640 | 未完整归档 |
| `helmet_n1088_1011.rknn` | `yolov8_det`，1088×1088 | 未完整归档 |
| `helmet_n1088_1012.rknn` | `yolov8_det`，1088×1088 | 未完整归档 |
| `yolo26n-depth-float.rknn` | `yolo26_depth`，640×640 | 不从文件名推定精度 |
| `zipdepth_npu_768x768-rk3588-fp16.rknn` | `zip_depth`，768×768 | 原记录为 FP16 |

## 基线结果

| 场景 | 成功推理吞吐 | 结果槽发布吞吐 | queue+worker 均值 | 进程 CPU 均值 |
| --- | ---: | ---: | ---: | ---: |
| 两路：ZipDepth + YOLO26 Depth | 14.75 帧/s 合计，分别 7.37 / 7.38 | 13.47 帧/s 合计，分别 6.77 / 6.70 | 两路约 422.45 ms，加权 | 273.03% |
| 六路：每路一个不同权重 | 33.24 帧/s 合计，各路 5.53–5.56 | 31.57 帧/s 合计，各路 5.13–5.35 | 559.72 ms，加权 | 336.33% |
| 一路：同帧六模型 | 5.30 **批/s** | 5.30 批/s | 588.79 ms | 213.29% |

CPU 100% 表示占用一个逻辑 CPU 核心；这里是引擎进程 utime+stime，不包含独立 FFmpeg 客户端、Web 和投递服务。三种场景有效统计窗口的任务 `failed` 均为 0，保留日志未发现匹配的模型异常/RKNN 错误；RTSP 解码持续推进，进程正常退出。**这是分钟级回归，不是长期稳定性数据。**

六路的逐模型差异、成功任务/发布计数及两次运行的波动见 JSON；不应只观察合计 FPS。一路六模型中的 5.30 是处理同帧多模型任务的批次数，不能写成单模型裸 NPU FPS，也不能与六路结果简单等同。

### 指标口径

- `infer_fps`：成功推理任务 / 实际窗口秒数。组合模型目前至少一个子模型成功即可将任务记成功；任务 `failed=0` 不能单独证明全部子模型每帧成功，应联合检查子模型结果和日志。
- `publish_fps`：结果通过序号检查、写入通道最新结果槽的次数；不是业务消费次数、RTSP/HDMI 播放 FPS。旧结果被更晚帧超过时会被丢弃。
- `queue+worker`：入队等待与成功任务处理均值，**不包含解码前、后续业务逻辑、渲染、网络和客户端播放**，不是完整端到端延迟，也没有 P50/P95。
- 日志中的 `rknn` 包含核心等待和 RKNN 输入/运行；输出获取等计入 `post`。并行子模型 `pre/rknn/post` 累加可能大于任务实际墙钟 `worker`，不能直接相加解释整体耗时。
- `[Feed]` 中限帧跳过、pending 淘汰和入队失败是不同事件；缺少完整统一分母时不合并成“丢帧率”。NPU 利用率快照也不能代替吞吐。

另外的[绘制微基准](performance-display-benchmark.md)只调用显示路径，不运行完整采集/推理/业务/RTSP 管线；它的循环 FPS 和 CPU 变化不能外推为系统推理性能。

## 运行现有测试程序

硬件测试前停止其他推理实例并确认端口空闲；占用锁会拒绝第二个本框架推理进程。使用隔离的测试应用包/配置，不在生产配置上做扫参。以下占位路径需替换为真实资源。

### 完整管线 A/B 对比

现有 `engine/tests/inference/run_npu_performance.py` 需要一份可运行应用包（含 libs/assets）、基础配置、模型节点数组 manifest，以及两个待比较的二进制。manifest 的结构可参考 [记录摘录中的 models 数组](data/2026-10-26-baseline.json)，只取数组本身，并替换实际文件。

从仓库根目录执行，两个版本须保持相同编译模式与 Runtime：

```bash
python3 engine/tests/inference/run_npu_performance.py \
  --baseline /path/to/baseline/vision_analysis \
  --candidate /path/to/candidate/vision_analysis \
  --app /path/to/test_app \
  --config /path/to/test_app/assets/test.json \
  --manifest /path/to/models.json \
  --output /path/to/new-comparison \
  --cases dual channels composite \
  --warmup 15 --seconds 60 --repeats 2
```

`dual` 直接保留基础配置的通道结构，必须由你准备双路配置；`channels` 将 manifest 各模型放到独立通道；`composite` 放到同一通道。runner 会使用端口 18554 并写独立配置/日志。默认 `--max-regression-percent=0`，任何一条通道的推理、发布或平均耗时回退都会写 `acceptance.json` 并返回 2；小幅运行波动也会触发，不应忽略失败或自动放宽阈值。

用于组合、绑核、热换与线程创建失败回退的板端脚本见 [run_model_matrix.py](../../engine/tests/inference/run_model_matrix.py)和 [NPU 调度文档](../../engine/src/vision/inference/NPU_SCHEDULING.md)。它们验证运行行为，不自动给出真实摄像头端到端延迟。

### 模型独立推理

独立工具位于 [`tools/device/npu_bench/`](../../tools/device/npu_bench/README.md)，由自己的 `build.sh` 使用锁定 SDK 构建，不参与主引擎的默认构建。在 RK3588、依赖已准备好时，从仓库根目录执行：

```bash
./tools/device/npu_bench/build.sh
./tools/device/npu_bench/npu_bench /path/to/model.rknn --mode latency --seconds 15 --repeats 3
./tools/device/npu_bench/npu_bench /path/to/model.rknn --mode throughput --instances 3 --json result.json
```

新版默认使用模型原生输入输出，输入数据和内存都提前准备好；计时包含模型执行中必需的 CPU/NPU 运算，不包含视频、RGA、业务编排、后处理和网络。`latency` 默认单实例、核心 0；`throughput` 默认三实例分配到核心 0/1/2。每轮先预热再计时，输出平均耗时、P50/P95 和窗口吞吐；`--io readback` 可额外测量输出取回。旧数字位置参数已取消，完整用法及动态尺寸限制见工具 README。

构建会校验 SDK，并为程序写入锁定 Runtime 的库搜索路径。逐层 profiling 工具放在 [`tools/device/rknn_profile/`](../../tools/device/rknn_profile/README.md)，通过自己的 `build.sh` 在原目录生成可执行文件。频率实验工具单独放在 [`tools/device/npu_freq/`](../../tools/device/npu_freq/README.md)，直接运行 `npu_freq.sh`，无需编译。

### 软件回归

在已经按首页构建的环境中，以下命令只启用并执行公共 NPU 调度的 CPU/报告测试，不运行真实模型：

```bash
cmake -S engine -B build/engine -DBUILD_NPU_TESTS=ON
cmake --build build/engine -j4 --target \
  npu_core_gate_tests npu_execution_tests \
  npu_combinations_tests npu_device_owner_tests
cd build/engine
ctest --output-on-failure -LE hardware -R '^npu_'
```

测试覆盖跨编译单元共享状态、81 种掩码配对、同核互斥/异核并行、公平性、停止/故障唤醒、输出释放、进程占用和逐路性能报告判断。它们不能代替实际模型、RGA、MPP 或摄像头验证。模型硬件入口测试由 `npu_model_inputs_tests` 提供，用法见调度文档。

## 待实测矩阵

| 项目 | 下一次实测必须补充 |
| --- | --- |
| 并发路数 | 1/2/4/6/8/15 路，明确哪些已达到资源上限；分别测同模型、混合模型与独立真实摄像头 |
| 端到端时延 | 摄像头时间基准、采集与客户端时间同步，报告 P50/P95；不能由平均推理时间推算 |
| 资源与丢帧 | CPU 逐进程口径、RSS/PSS/CMA/FD 长时曲线，分列采集丢失、限频、队列淘汰、过期结果及编码丢弃 |
| 环境 | 资产型号/RAM、散热器/风扇、室温、供电、CPU/NPU/DDR governor 与频率 |
| 可复现资产 | 权重/视频哈希、取得方式与授权，RKNN 输入输出/量化清单、完整脱敏配置和原始日志 |
| 稳定性 | 明确持续时长，注入网络断开/恢复、视频异常、磁盘满、进程与驱动故障，统计每类错误与恢复时间 |
| 适配覆盖 | YOLO26 Pose 的真实权重混跑；YOLOv8 Pose / YOLOv5 Seg 的更多尺寸变体、精度与长时测试；其他 BSP 和摄像头组合 |

## 历史文档验收

此节为较早 README 重构时的验收记录；之后的真实 RTSP / USB 对照见上方新报告，与此阶段分开：

- 已核对构建/安装脚本、模型工厂和校验器、配置解析、队列与核心调度、预处理回退、编解码、事件和 SDK 入口。
- 本机只读环境检查：53 项通过，0 警告、0 失败；Logic 清单：28 个通道、2 个全局模块校验通过。
- 最小配置与首页推理数组均通过 C++ 配置校验；在隔离的临时应用目录运行各 10 秒 RTSP，纯视频解码 292 帧、启用 YOLOv8 检测后解码 303 帧，均正常退出，推理窗口成功完成并发布且 `failed=0`。使用本机既有视频、权重、同一构建二进制与锁定 Runtime，测试只将输出端口改为 18554；这是冒烟验证，不是新的吞吐 Benchmark。
- 项目指南的完整 ROI 配置例子通过 C++ 校验；最小业务模块用公开 SDK 头文件通过 C++ 语法编译。模型检查 CLI 返回成功。
- 在独立临时构建目录配置并构建公共调度测试，10 项 `npu_` CTest 全部通过；裸 NPU 程序用锁定 SDK 成功编译，未执行负载。
- Markdown 代码块、JSON、Shell 语法和相对链接/锚点经过检查，项目愿景与原文逐字一致。检查记录当时写入本机 `/tmp/rk3588-readme-review/`；临时目录可能已清理，不作为公开运行素材。
- 本次未执行系统重装、联网依赖安装、离线制包、Docker 交叉构建、GPIO 动作、OTA、真实摄像头及长期压测；这些仍需对应设备/系统验收。未生成运行截图或新的性能宣传数据。
