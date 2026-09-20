# crane_safety

四个检测模块与一个全局控制器共同组成吊机应用。新配置默认停用通道和硬件输出；请配置实际视频源、模型、ROI 后启用通道，再将通道 0–3 连接到全局控制器并启用联动。原仓库没有安全帽专用模型，需自行提供。

```bash
../../rkvision build .
../../rkvision package .
# 或直接使用 CMake
cmake -S . -B build/direct -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct --parallel 4
```

打包产物在 `dist/`，将 `.tar.gz` 上传 Web 后选择配置并启动。
项目可移出框架仓库：使用对应框架目录下的 `rkvision` 命令。

初始配置未启用任何通道，配置校验和启动会提示 `no enabled channels`；请在 Web 配置并启用通道后运行。
