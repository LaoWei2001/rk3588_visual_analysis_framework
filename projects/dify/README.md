# dify

提供 assets/input.mp4，并在 Web 配置事件上报策略与 Dify 连接后运行。

```bash
./build.sh
./build.sh package
# 或直接使用 CMake
cmake -S . -B build/direct -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct --parallel 4
```

打包产物在 `dist/`，将 `.tar.gz` 上传 Web 后选择配置并启动。
项目可移出框架仓库：通过 `RKVISION_ENGINE` 环境变量或 `--engine` 指定引擎目录。
