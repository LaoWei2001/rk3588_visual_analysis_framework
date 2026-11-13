# ZipDepth 视频与算法开发

底层实现位于 `engine/src/vision/models/zip-depth/`，模型类型为 `zip_depth`。
使用与 YOLO 相同的模型节点、通道、多模型结果列表和业务回调。
当前 768×768 FP16 模型及元数据已放到 `projects/assets/`，随项目一起打包。

在仓库根目录编译后运行示例：

```bash
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine -j4 --target vision_analysis
cd projects
../build/engine/vision_analysis assets/zip_depth_video.json
```

示例读取 `assets/10_11_1.mp4`，使用三个推理 worker，关闭本地窗口，开启 `rtsp://板卡IP:8554/live` 深度预览。
按 Ctrl+C 退出。`global.channel_threads` 可改为 1 以降低上下文内存占用；三线程的实测推理吞吐约 11–12 次/秒，发布帧率约 10–11 帧/秒（关闭显示和 RTSP 的测试条件）。通过 Web 使用时，在新版控制台选择 `zip_depth`、导入模型文件，
将模型连接到通道的 `logic_zip_depth_demo`；无需标签文件。已安装的控制台副本需要更新后才显示新类型。
替换视频源和添加检测模型的方法与原有 YOLO 一致。

## 统一读取结果

```cpp
#include <channel.h>

static void logic_example(ChannelContext *ctx)
{
    if (!ctx) return;
    const AlgoResult *depth = ctx->depth_result("zip_depth"); // 配置中的模型 ID
    if (!depth || !depth->is_relative_inverse_depth()) return;
    if (depth->depth_valid_at(320, 320)) {
        const float value = depth->depth_at(320, 320);
        // value 是相对逆深度，越大越近；不能当成米，也不能直接 1/value 换算米。
        (void)value;
    }
    for (const auto &target : *ctx->results) {
        if (target.is_depth()) continue;
        const float relative = depth->depth_median(target.box);
        // 适合比较同一帧、同一模型内的相对远近。
        (void)relative;
    }
}
REGISTER_LOGIC(logic_example);
```

`ctx->results` 中每个 ZipDepth 模型每帧一条 `AlgoResult`，包含 `depth_map`、
`depth_kind` 和统一的模型 ID、通道 ID、帧号、时间戳。
`depth_map` 是 640×640 的 `CV_32FC1`，与检测框、姿态点、ROI 使用同一坐标系。
`depth_at()` 与 `depth_median()` 返回原始值，无有效值时返回 0；相对深度的有效 0
可用 `depth_valid_at()` 区分。ROI 外部原始矩阵为 NaN。米制 YOLO26 继续用 `Metric`，
原 `logic_depth_demo` 已增加类型检查，不会把 ZipDepth 的相对值作为距离上报。

`depth_result()` 指针只在当前回调有效；`cv::Mat saved = depth->depth_map` 可跨帧保留，
后续推理不会覆盖它。需要改写矩阵时先 `clone()`。

## 模型尺寸选择

当前输入转换的 NEON/原生 FP16 优化、加载时数值校验及实机性能边界见
[ZipDepth 性能排查](../../../docs/performance/zip-depth-performance.md)。RKNN 内部仍有 CPU 算子，
开启三核或使用 RGA 输入不等于整个网络都由 NPU 执行。

切换配置中的 `model_path` 或在 Web 选择其他尺寸的 RKNN 文件即可，模型尺寸自动读取。
可以让 672×384 ZipDepth 与 640×640/1088×1088 YOLO 组合，二者各自从原视频取图。
视频分辨率、模型输入分辨率和统一业务坐标彼此独立，不需要在业务代码中补偿模型尺寸。

当前只附带 768×768 模型。384×384、384×672（H×W）需提供对应导出文件；
输入仍必须是 RGB uint8，并且模型已经包含 /255 和 ImageNet 归一化。
输入分辨率会影响推理细节、内存与速度，应使用实际场景图片比较结果。

ROI 单独推理和整帧结果过滤均可用；“整帧＋ROI”模式对 ZipDepth 保留整帧深度，
避免把不同裁剪的相对值拼成一张不一致的图。相对深度不宜直接跨帧、跨模型作绝对距离比较。
