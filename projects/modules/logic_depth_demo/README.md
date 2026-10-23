# 视频深度推理与二次开发

`yolo26_depth` 已作为原生 `ModelBase` 接入框架，使用与检测、姿态、分割相同的配置、
视频解码、推理线程、模型热重载、结果分发和业务 Logic 机制。无需在业务回调中加载模型、
写取流循环或调用 RKNN。源码示例只包含公开 `<channel.h>`。

## 启动已部署的示例

仓库根目录运行：

```bash
# 单深度模型，三个推理线程，默认读取项目已有的 10_11_1.mp4
./depth/run_video.sh

# 同一视频、同一帧：1088 检测模型 + 640 深度模型
./depth/run_video.sh projects/assets/depth_multi_model.json
```

已编译产物为 `build/engine/vision_analysis`。后续修改源码需执行 `./build.sh --debug`，
再运行上述脚本。停止按 Ctrl+C。配置文件分别是 `projects/assets/depth_video.json` 和
`projects/assets/depth_multi_model.json`；已有模型已复制到
`projects/assets/yolo26n-depth-float.rknn`，随项目 assets 一起打包。

示例默认不打开本地桌面窗口，开启 RTSP 预览：`rtsp://板卡IP:8554/live`。
用现有 Web 预览或 VLC 等客户端播放，可看到深度热力图及中心距离。
需要本地显示时，按现有方式开启 `global.enable_display`；关闭推流设 `global.enable_rtsp=false`。
启用深度模型的通道以纯深度热力图作为视频底图，近处红、远处蓝，不再混入原图。
无效/未推理区域显示黑色，首个深度结果到达前也使用黑底；检测框、ROI 和业务绘图
按既有设置叠加在热力图上。该规则统一应用于视频窗口、RTSP 和启用绘图的截图/录像。
推理及业务读取的 `model_frame()/source_frame()` 仍提供原始 BGR，浮点深度数据不受显示方式影响。

更换实时摄像头时直接修改现有 `stream` 节点，例如：

```json
"stream": {
  "src_type": "rtsp",
  "url": "rtsp://你的摄像头地址",
  "video_enc": "h264"
}
```

Web 画布中的模型节点可选 `yolo26_depth`，选择 RKNN 文件后，像其他 YOLO 一样连接到
通道 Logic。深度模型无需标签文件、置信阈值或 NMS 阈值。可选择 `logic_depth_demo` 查看
距离，也可在自己的模块直接读取以下结果。

本机当前 systemd 控制台使用 `/opt/ai_apps/_console` 的独立部署副本；源码中的 Web 改动
需随新版控制台部署后才会显示。上述启动脚本直接使用本次编译的引擎，不依赖该控制台副本。
控制台更新沿用根目录安装器的部署流程。

## 业务代码如何获取结果

```cpp
#include <channel.h>

static void logic_my_depth(ChannelContext *ctx)
{
    if (!ctx) return;
    // 不传 ID 取本帧首个深度模型；也可用 ctx->depth_result("depth")。
    const AlgoResult *depth = ctx->depth_result();
    if (!depth) return;

    const float center_m = depth->depth_at(320, 320);
    const float region_m = depth->depth_median(cv::Rect(240, 240, 160, 160));

    for (const auto &target : *ctx->results)
    {
        if (target.is_depth()) continue;
        const float target_m = depth->depth_median(target.box);
        // target_m > 0 时，得到本帧目标框内的有效深度中位数（米）。
        // 可继续用现有 draw_*、publish_*、report_event() 等业务接口。
        (void)target_m;
    }
    ctx->publish_number("center_depth_m", center_m);
    (void)region_m;
}
REGISTER_LOGIC(logic_my_depth);
```

自己的 `logic.json` 中需声明对应的 `outputs` 和业务参数，规则与其他模块相同；
可直接从本目录的示例开始修改。

| 接口 | 语义 |
| --- | --- |
| `ctx->depth_result("模型ID")` | 当前结果批次中的深度结果，未就绪返回 `nullptr` |
| `result.is_depth()` | 区分深度帧结果与检测/姿态等目标 |
| `result.depth_map` | `CV_32FC1` 深度矩阵，单位米 |
| `result.depth_at(x,y)` | 获取像素深度；无效、越界时返回 `0` |
| `result.depth_median(rect, step=4)` | 有效像素的区域中位数；无有效值时返回 `0` |

`depth_map`、检测框、姿态点和 ROI 均使用项目的 **640×640 业务坐标系**。模型实际输入
可以是 640、1088 或 rect 尺寸，框架负责映射；无需用原视频分辨率再次缩放检测框。
从原图坐标读取时，先转换到业务坐标，例如 `x*640/src_width`、`y*640/src_height`。

一帧中每个深度模型只产生一条携带整张深度图的 `AlgoResult`。它含有与其他模型相同的
`model_id/model_type/model_index/chn_id/frame_id/timestamp_ms`，可以直接遍历 `ctx->results`。
深度图会随同帧视频和目标一起分发，不做跨帧拼接。`target_count()`、`query_targets()`、
类别过滤、NMS、SORT 和 ByteTrack 均不会把深度图当成检测目标。

## ROI、数据生命周期与显示

- 支持既有整帧、`roi_only`、`full_plus_roi`、`full_frame_roi_filter` 和 ROI resize 模式。
  未覆盖区域写 0；整帧加 ROI 模式下，ROI 内的有效深度覆盖整帧深度，每个模型仍只保留一张图。
- `ctx->depth_result()` 指针只在当前回调有效。需要跨帧保留时复制 Mat 头部：
  `cv::Mat saved = depth->depth_map;`，后续推理不会覆盖其像素；业务若要改写像素，请先 `clone()`。
- `get_channel_frame_snapshot()` 与其他图像/结果一样，按需深拷贝深度数据，支持同帧证据和全局逻辑。
- 读取深度不会触发 `model_frame()/source_frame()`，热路径无需生成 CPU BGR 视频图像。
- `depth_median()` 默认步长 4，减少每帧大量检测框查询的 CPU 开销；需要每像素参与时传 `step=1`。
  偶数样本使用两个中间值的平均值。零值、负值、NaN、Inf 不参与。

## 性能处理与实测

- 硬件解码缓冲直接经 RGA 缩放、转 RGB 写入 RKNN 输入 DMA，复用目标 RGA 句柄。
- CPU 回退复用缩放缓冲，并直接写入输入 DMA，避免 vector 中转和重复归一化。
- RKNN 将浮点输出填入有界缓存池；只有没有发布读者的缓冲才会复用。640 模型映射到 640
  业务画布时仅传 Mat 引用，避免每帧复制和上采样到完整 1080p。
- 热力图只在显示/媒体出口需要时生成，并按推理结果缓存；不增加推理工作线程的着色开销。
- `threads>1` 且深度模型 `npu_core=-1` 时，工作实例轮流绑定 RK3588 的 0/1/2 核。
  显式 `npu_core` 设置始终保留；线程和模型热重载使用同一策略。
- 沿用队列大小 1 的最新帧策略，超负荷时淘汰待处理旧帧，避免延迟无限累积。

本机 RK3588、Runtime 2.4.2a2、默认系统调频设置，项目已有视频的实测：

| 场景 | 推理吞吐 | 业务发布吞吐 | 预处理 |
| --- | --- | --- | --- |
| 深度，1 个工作线程 | 约 6.3 FPS | 约 6.3 FPS | 约 1.1 ms/帧 |
| 深度，3 个工作线程 | 约 19 FPS | 约 17–19 FPS | 约 1.1 ms/帧 |
| 1088 检测 + 640 深度，各绑 1 个核 | 约 6.3 FPS | 约 6.3 FPS | 两个模型合计约 3 ms/帧 |

多线程提高吞吐，并不把单帧 NPU 延迟降到 `1000/FPS`。以上基准关闭显示和 RTSP 编码，
开启预览、增加模型/通道、温度和频率变化都会影响实际速度；未更改系统 governor。
日志中的 `[Perf]` 同时给出 `infer_fps/publish_fps/pre/rknn/post`，可在你的摄像头上对比。

已验证真实 NPU 输出与原先图片推理 FP32 结果最大误差为 0，独立持有 8 帧不会被覆盖，
ROI/业务坐标映射、目标查询、混合模型、快照和显示叠加有回归测试。

这是单目模型的米制估计，距离准确度仍取决于场景及模型训练分布。当前提供的模型为
640×640；非方形视频会按上游预处理拉伸，匹配相机比例的 rect RKNN 可以减少几何失真。
无需为“实时”再执行一次 `exp`、取倒数或 `/255`；这些处理已由模型和输入接口负责。

## 重新构建与检查

```bash
./build.sh --debug
# 正式应用仍按原发布流程打包，模型/示例配置已经放入 projects/assets/
./build.sh my_app
```

开发测试可按需开启：

```bash
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_DEPTH_TESTS=ON -DBUILD_SDK_TESTS=ON -DBUILD_INFERENCE_ROI_TESTS=ON \
  -DBUILD_FRAME_SNAPSHOT_TESTS=ON
cmake --build build/engine --parallel 4
cd build/engine
ctest --output-on-failure
```

RKNN 硬件测试默认使用仓库根目录的原模型和 `depth/assets/bus.jpg`，可通过
`DEPTH_TEST_MODEL/DEPTH_TEST_IMAGE` 指定。缺少测试文件时不注册硬件测试，不影响产品构建。
