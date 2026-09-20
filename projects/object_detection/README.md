# object_detection

保留原 config_test.json 以及通用检测、分割和姿态模型，使用前检查现场摄像头地址。

```bash
../../rkvision build .
../../rkvision package .
# 或直接使用 CMake
cmake -S . -B build/direct -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct --parallel 4
```

打包产物在 `dist/`，将 `.tar.gz` 上传 Web 后选择配置并启动。
项目可移出框架仓库：使用对应框架目录下的 `rkvision` 命令。
