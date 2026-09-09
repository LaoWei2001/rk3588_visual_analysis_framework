# YOLO26 Pose RKNN 验证工具

这是一个独立于主程序的 RK3588 验证程序，用来检查 Ultralytics 导出的 YOLO26 Pose RKNN 模型，并在图片、视频或摄像头画面上绘制人体框、17 个 COCO 关键点和骨架。

当前默认适配你的模型输出 `(1, 56, 8400)`：

- 4 个框坐标：`cx, cy, width, height`
- 1 个类别置信度：`person`
- 17 × 3 个关键点值：`x, y, visibility`

因为 RKNN 导出关闭了 end-to-end 分支，程序在 CPU 上执行置信度过滤和 NMS。

同时支持 `(1, 300, 57)` 的 end-to-end Pose 输出：每条结果为
`x1, y1, x2, y2, score, class_id, 17×(x,y,visibility)`，这种输出无需再次执行 NMS。

## 编译

在 RK3588 目标机执行：

```bash
cd /userdata/rk3588_visual_analysis_framework/yolo26_pose_rknn_verify
chmod +x build.sh
./build.sh
```

依赖是 C++17、CMake、OpenCV 4 和 RKNN Runtime。CMake 会使用项目已有的：

```text
../vision_analysis/vendor/rknn/2.4.2a2/include/rknn_api.h
../vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64/librknnrt.so
```

构建后，该 `librknnrt.so` 会复制到 `build/lib/`，程序通过相对 RPATH 加载它。

## 使用

先只检查模型和运行时：

```bash
./build/yolo26_pose_verify \
  --model /userdata/rk3588_visual_analysis_framework/yolo26n-pose-rk3588.rknn \
  --inspect
```

验证图片：

```bash
./build/yolo26_pose_verify \
  --model /userdata/rk3588_visual_analysis_framework/yolo26n-pose-rk3588.rknn \
  --source /path/test.jpg \
  --output output/test_pose.jpg
```

验证视频：

```bash
./build/yolo26_pose_verify \
  --model /userdata/rk3588_visual_analysis_framework/yolo26n-pose-rk3588.rknn \
  --source /path/test.mp4 \
  --output output/test_pose.mp4
```

验证摄像头：

```bash
./build/yolo26_pose_verify \
  --model /userdata/rk3588_visual_analysis_framework/yolo26n-pose-rk3588.rknn \
  --source 0 --show \
  --output output/camera_pose.mp4
```

服务器或无桌面环境不要加 `--show`，结果仍会保存。按 `Q` 或 `Esc` 退出实时预览。完整参数使用：

```bash
./build/yolo26_pose_verify --help
```

## 注意

- RKNN 模型只能在带有可用 RKNPU 驱动的 RK3588 等目标平台实际推理。
- 导出使用的 `rknn-toolkit2 2.3.2` 与这里随项目提供的 Runtime `2.4.2a2` 不是完全相同的版本。程序启动时会打印 Runtime API 和内核驱动版本；如果模型初始化失败，应首先统一 Toolkit、Runtime 和驱动版本。
- 此目录用于独立验证，还没有把 YOLO26 的单输出解码器接入现有 `vision_analysis` 主程序。现有主程序的旧 Pose 解码器使用多输出结构，不能直接替代。
- 如果以后导出的是多类别或不同关键点数量的模型，请分别通过 `--classes` 和 `--keypoints` 指定。
