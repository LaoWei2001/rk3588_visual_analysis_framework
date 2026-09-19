# 独立视觉项目

本目录是 `vision create` 使用的最小项目模板。逻辑、资源、配置和构建产物均属于本项目。

```bash
./build.sh                        # Release 编译
./build.sh --build-type Debug      # 调试编译
./build.sh package                # 生成 dist/<项目ID>/ 和同级 tar.gz
./build.sh clean                  # 删除当前 Release 构建缓存
```

也可直接使用 CMake：

```bash
cmake -S . -B build/direct -DRKVISION_ENGINE=/path/to/framework -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct --parallel 4
```

引擎选择：`build.sh --engine` / CMake `-DRKVISION_ENGINE`，或 `RKVISION_ENGINE` 环境变量。
创建工具会写入本机的 `engine.local.cmake`（不提交 Git）。仓库 `projects/` 内的项目可自动找到共用引擎。
自定义编译和链接依赖添加在本项目的 `CMakeLists.txt` 中。

配置 `project.json` 的引擎版本与 SDK API。入口逻辑使用 `<rkvision/logic.h>`，全局逻辑使用
`<rkvision/global_context.h>`；业务代码不依赖引擎私有头文件。
初始配置读取 `assets/input.mp4`，请提供视频或在 Web 修改视频源后运行。
发布时上传 `dist/` 下的应用压缩包，在 Web 选择配置并启动。

公共引擎以 `libs/librkvision.so.1` 随应用交付，业务编译到 `vision_analysis`。
`build.sh` 先增量构建共用引擎缓存，再编译本项目；直接 CMake 默认在本项目构建目录生成共享库。
请部署整个 `dist/<项目ID>/` 或上传同级压缩包，进入发布目录执行 `./run.sh` 即可启动。
兼容的引擎内部修复可通过更新库并重启生效；SDK/编译器 ABI 变化需要重编译项目。
