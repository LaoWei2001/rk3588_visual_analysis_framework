# relay_control

继电器项目默认停用通道。请设置视频源、引脚与电平后按需启用。

```bash
./build.sh
./build.sh package
# 或直接使用 CMake
cmake -S . -B build/direct -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct --parallel 4
```

打包产物在 `dist/`，将 `.tar.gz` 上传 Web 后选择配置并启动。
项目可移出框架仓库：通过 `RKVISION_ENGINE` 环境变量或 `--engine` 指定引擎目录。

初始配置未启用任何通道，配置校验和启动会提示 `no enabled channels`；请在 Web 配置并启用通道后运行。
