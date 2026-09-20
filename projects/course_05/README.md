# course_05

独立课程项目，仅包含 `logic_course_05`。请提供 `assets/input.mp4`，或在 Web 中修改实际视频源。

```bash
cd projects/course_05    # 从框架根目录进入
../../rkvision build .
../../rkvision package .
```

也可直接执行 `cmake -S . -B build/direct && cmake --build build/direct`。
项目移出仓库时，用 `/path/to/framework/rkvision build .` 指定共用引擎。
将 `dist/course-05.tar.gz` 上传 Web 的程序管理页面，配置后启动。
