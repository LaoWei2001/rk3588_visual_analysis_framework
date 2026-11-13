# 模型内部耗时分析工具

`rknn_profile` 用来查看：**一个模型执行时，内部哪些步骤比较耗时，是否有步骤在 CPU 上执行。**

模型由很多计算步骤组成，通常称为“层”或“算子”。这个工具让 RKNN Runtime 输出这些步骤的
性能明细，适合排查模型为什么慢。只想测模型每秒能执行多少次时，先用
[npu_bench](../npu_bench/README.md)。

## 第一次使用，照这几步操作

### 1. 进入目录，准备程序

在 RK3588 设备上执行：

```bash
cd /userdata/rk3588_visual_analysis_framework/tools/device/rknn_profile
ls -l ./rknn_profile
```

下面的命令都在这个目录执行。仓库放在其他位置时，修改上面的路径。
如果已有 `rknn_profile`，直接使用；没有文件或修改了源码，再运行：

```bash
./build.sh
```

编译成功后，程序就生成在当前目录。测速前，先在 Web 控制台或相应终端正常停止其他推理程序，
避免占用同一个 NPU。

### 2. 选择模型，执行分析

以项目中的 ZipDepth 模型为例，先检查文件是否存在：

```bash
ls -lh ../../../projects/assets/zipdepth_npu_768x768-rk3588-fp16.rknn
```

有这份模型时可以执行：

```bash
sudo ./rknn_profile ../../../projects/assets/zipdepth_npu_768x768-rk3588-fp16.rknn 5
```

没有这份模型时，把模型路径换成自己已有的、适用于 RK3588 的 `.rknn` 文件。
可以使用完整路径；路径包含空格时需要加引号。

**最后的 `5` 表示测量 5 次，不是运行 5 秒。** 程序先执行 3 次预热，让模型运行起来，
再统计后面 5 次的平均耗时，最后自动退出。模型越慢，等待时间越长。

不需要准备图片或视频，程序会自己生成固定灰色输入。它只检查执行耗时，不检查识别精度。

## 参数怎么填

```text
./rknn_profile 模型文件 测量次数
```

| 参数 | 含义 | 第一次使用怎么填 |
| --- | --- | --- |
| 模型文件 | 要分析的 `.rknn` 模型 | 换成自己的模型路径 |
| 测量次数 | 计入平均耗时的执行次数，必须大于 0 | 先填 `5`，需要更多样本时可填 `10` |

测量次数省略时默认为 `5`。这个工具固定使用 **NPU 核心 0**，没有选择其他核心的参数。

## 输出怎么看

开头会有一行汇总。下面的数字只是**格式示例，不是实测成绩**：

```text
PROFILE MODE, core 0, RGB8 DMA: run=80.000 ms, get=2.000 ms (5 samples after 3 warmups)
```

| 输出 | 怎么理解 |
| --- | --- |
| `PROFILE MODE` | 开启了详细性能统计；统计本身会影响执行速度 |
| `core 0` | 本轮固定使用 NPU 核心 0 |
| `RGB8 DMA` | 程序使用 8 位 RGB 图像数据，通过设备可访问的内存送入模型；无需自己准备这种输入 |
| `run=80.000 ms` | 每次模型执行调用平均耗时 80 毫秒，这段时间可能包含模型中的 CPU 计算 |
| `get=2.000 ms` | 每次把模型结果取回程序，平均需要 2 毫秒 |
| `5 samples after 3 warmups` | 先预热 3 次，再测量 5 次 |

后面会接着打印 RKNN Runtime 提供的逐层报告，字段和单位以实际表头为准。
先找耗时较长的计算步骤；如果报告标出某一步在 CPU 上执行，说明它没有完全交给 NPU。
CPU 上有步骤不代表模型执行失败，但可以作为后续优化的排查线索。

开头的 `run`、`get` 是多次测量的平均值；后面的逐层报告直接来自 Runtime，
程序没有对它做多次平均。也不要把 `1000 ÷ run` 当成真实视频 FPS：这里开启了额外统计，
又只使用一个 NPU 核心，并且没有视频解码、图像处理或业务逻辑。

## 想把报告保存下来

在同一目录执行：

```bash
sudo ./rknn_profile ../../../projects/assets/zipdepth_npu_768x768-rk3588-fp16.rknn 10 > profile.log 2>&1
```

这会测量 10 次，把输出和报错都写入当前目录的 `profile.log`，终端暂时不显示过程。
完成后查看：

```bash
less profile.log
```

按方向键翻看，按 `q` 退出查看。再次使用相同的保存命令会覆盖旧日志，需要保留时先换个文件名。

## 常见问题

| 现象或提示 | 含义与处理方法 |
| --- | --- |
| `./rknn_profile: No such file or directory` | 确认当前目录，再执行 `./build.sh` |
| `NPU is owned by another inference process` | 其他框架应用或测试正在占用 NPU，先停止它 |
| `Cannot open NPU ownership lock: Permission denied` | 使用 `sudo` 运行 |
| `iterations must be positive` | 测量次数必须是大于 0 的整数 |
| `requires one image input and nonempty outputs` | 模型需要恰好一个图像输入，并且至少有一个输出 |
| `requires static batch-1 RGB input`、`invalid RGB shape` | 模型需要固定尺寸、每次一张、三个颜色通道的图像输入；当前程序不适合这个模型形状 |
| `init: ...` | 模型初始化失败；先检查路径，再检查模型是否适用于当前 RK3588 和驱动 |
| `run: ...`、`get: ...`、`query profile: ...` | 执行或获取报告失败，保留完整日志排查，本轮没有得到有效的完整分析 |

编译需要 C++14 编译器，默认使用 `g++`，并校验、链接仓库自带的 RKNN SDK `2.4.2a2`。
模型输入可以按“高、宽、颜色通道”或“颜色通道、高、宽”排列（NHWC/NCHW），
但都必须满足上述单图、三通道、固定尺寸要求。移动仓库后需重新编译。

使用本工具分析 ZipDepth 的例子见 [ZipDepth 性能说明](../../../docs/performance/zip-depth-performance.md)。
