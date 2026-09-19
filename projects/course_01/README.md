# course_01

独立课程项目，仅包含 `logic_course_01`。请提供 `assets/input.mp4`，或在 Web 中修改实际视频源。 默认配置保留原来的无推理入门行为；`assets/config_inference.json` 是启用推理的课程配置。

```bash
cd projects/course_01    # 从框架根目录进入
./build.sh
./build.sh package
```

也可直接执行 `cmake -S . -B build/direct && cmake --build build/direct`。
项目移出仓库时，用 `--engine /path/to/framework` 或 `RKVISION_ENGINE` 指定共用引擎。
将 `dist/course-01.tar.gz` 上传 Web 的程序管理页面，配置后启动。
