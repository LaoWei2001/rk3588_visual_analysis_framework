# 独立视觉项目、SDK 与共享引擎

## 目录与所有权

`projects/` 下一个目录对应一个应用，所有项目直接并列。当前有 `crane_safety`、`person_count`、
`object_detection`、`dify`、`relay_control`，以及 `course_01` 至 `course_10`、`course_gpio`，共 16 个项目。
原先合并的参考应用已拆分删除；19 个通道模块、2 个全局模块各有唯一所属项目。
全部原资源已保留，有共同需要的模型在各项目内保存，以便项目独立移动和发布。

```text
projects/my_app/
├── CMakeLists.txt       # 本项目的 CMake 根工程，引用共享引擎
├── build.sh             # 编译、打包和清理入口
├── project.json         # 应用 ID、版本、引擎契约、默认配置
├── logic/
│   ├── modules/         # 通道业务
│   ├── global_modules/  # 跨通道业务
│   └── common/          # 可选的项目内部公共代码
├── assets/              # 配置、模型、标签等资源
├── report_templates/    # 可选项目级上报模板
├── build/               # 自动生成，不提交
└── dist/                # 自动生成的交付包，不提交
```

一个项目可以含多种 logic。例如吊机项目同时包含运动、吊钩、安全帽、入侵检测与全局联动。
引擎、SDK、Web 和通用服务只由框架维护。业务项目不复制引擎 `src` 或 `logic/core`。

## 创建、编译与打包

从框架根目录创建：

```bash
./vision create projects/my_app
cd projects/my_app
./build.sh                       # Release
./build.sh --build-type Debug     # 调试构建
./build.sh package               # 生成 dist/my_app 与 dist/my_app.tar.gz
./build.sh clean                 # 清理本项目当前 Release 缓存，保留共享引擎缓存
```

`build.sh` 调用共用的 Shell 构建流程，先用 CMake 在框架 `build/runtime/` 增量编译
`librkvision.so.1`，再编译本项目的业务、入口和参数清单，链接成 `vision_analysis`。
多个项目复用同一份引擎构建缓存；构建期间使用文件锁保护共享库的生成与复制。
本项目的 `build/<构建标识>/libs/` 保存匹配的运行库，方便直接运行和打包。
`CMakeLists.txt` 用 `add_subdirectory` 引入目标定义。
Python 用于清单校验、项目生成和打包；原 `build.py` 已删除。

也可以直接使用标准 CMake：

```bash
cmake -S . -B build/direct -DRKVISION_ENGINE=/path/to/framework -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct --parallel 4
# 将上述构建打包；会核对项目、引擎和构建类型
/path/to/framework/vision package . --build-dir build/direct
```

新增模块可执行 `/path/to/framework/vision new-logic . channel logic_custom`。
`./vision check <项目>` 检查版本和模块 schema，不代替摄像头、模型及运行配置验收。
SDK 业务目标为 `vision_app`，最终链接目标为 `vision_analysis`。项目额外依赖直接添加到本项目
`CMakeLists.txt` 对应目标；只给业务公开 SDK、项目及 OpenCV 的 include 路径。

## 外部项目与引擎选择

项目目录可以放在任何位置，使用独立 Git 仓库：

```bash
/path/to/framework/vision create /userdata/my_app
cd /userdata/my_app
./build.sh --engine /path/to/framework
./build.sh package --engine /path/to/framework
```

选择优先级：显式 `--engine` / CMake `-DRKVISION_ENGINE`，环境变量 `RKVISION_ENGINE`，
创建工具写入的本机 `engine.local.cmake`，最后尝试仓库 `projects/<应用>` 的相对位置。
`engine.local.cmake` 不提交 Git；它只保存本机引擎路径，项目版本要求保存于 `project.json`。
构建缓存按引擎路径、提交、工具链、镜像和构建类型隔离。

非 ARM 主机默认通过 `rk3588_builder:2026_4_30` Docker 镜像交叉编译，支持 `--image`；
自备工具链使用 `--toolchain /path/to/cross.cmake`，直接 CMake 使用 `-DCMAKE_TOOLCHAIN_FILE`。
Shell 构建在 Linux、WSL 或提供 Bash 的环境执行；原生 Windows 可使用配置好交叉工具链的 CMake。

## 版本与升级

```json
{
  "id": "my-app",
  "version": "1.0.0",
  "engine": {"version": "1.0.0", "sdk_api": 1},
  "default_config": "assets/config.json"
}
```

引擎版本须与 `VERSION` 一致，SDK API 当前为 1。可额外指定完整 `engine.commit`，此时要求
引擎工作树干净。发布包 app.json 记录引擎版本、提交与 dirty 状态；源码归档标为 source-archive。
升级时更新引擎及项目的版本声明，重新编译、验收、打包，不搬运业务目录。
运行库的 SONAME 当前是 `librkvision.so.1`。CMake 与启动入口核对 ABI 主版本、SDK 头文件摘要、
编译器和 OpenCV 版本等兼容信息。SDK 仍暴露 C++/OpenCV 类型，不能跨任意编译器、libstdc++、
OpenCV 或 SDK 数据布局混用；改变公开 ABI 时应升级 ABI 主版本并重编译应用。
这项检查用于发现已知不匹配，不能替代 ABI 审查或目标系统依赖验收。

公开接口位于 `vision_analysis/include/rkvision/`：`logic.h` 提供通道常用接口，
`global_context.h` 提供全局上下文；`events.h`、`gpio.h`、`text.h` 分别提供事件、GPIO 与文字接口。
原有类型、数据布局和调用方式保持，运行时不加载业务插件。

## 引擎与应用产物

```text
framework/
├── vision_analysis/
│   ├── include/rkvision/       # SDK，业务使用的公开头文件
│   ├── src/                   # 共享引擎实现：解码、推理、显示、录像等
│   ├── application/main.cpp   # 每个应用编译一次的启动入口
│   ├── metadata/catalog.json  # 通用模型类型元数据
│   └── CMakeLists.txt          # rkvision 共享库及应用目标
├── build/runtime/             # build.sh 共用的引擎构建缓存
├── projects/                  # 每个业务项目直接并列
├── templates/basic_project/   # 新项目模板
├── tools/project/             # 共用构建、打包和安装工具
└── web_console/               # 独立 Web 管理平台
```

`vision_analysis` 可执行文件只包含本项目 logic、入口和嵌入式参数清单。
采集、推理、跟踪、显示、录像、全局调度等公共实现位于 `librkvision.so.1`。
它在进程启动时由系统加载；业务回调依旧是进程内函数调用，帧处理不增加序列化或额外复制。

发布包结构：

```text
person-count/
├── vision_analysis          # 本项目业务可执行程序
├── libs/librkvision.so.1     # 本包匹配的公共引擎
├── libs/                    # RKNN 和打包策略覆盖的其他直接依赖
├── assets/                  # 本项目配置、模型、标签
├── logics.json              # Web 使用的本项目清单
├── app.json                 # 版本、引擎 ABI、库 SHA256 等
├── run.sh                   # 设置路径后启动
└── services/                # 通用上传、模型更新服务
```

部署完整目录或 `.tar.gz`，不能只复制可执行文件。包内保留自己的引擎副本，方便独立升级和回滚；
当前没有把所有已安装应用强制绑定到 `/usr/lib` 中的一份全局引擎。
`--no-bundle-libs` 仍包含 `librkvision.so.1`，只省略其他第三方依赖；这些依赖须由目标系统提供。
默认打包直接依赖，系统底座仍须按部署文档安装，包不是独立操作系统镜像。

## 单独构建、复用和更新引擎

通常只需在项目执行 `./build.sh package`，脚本会自动更新共享缓存。也可以独立构建引擎：

```bash
# 框架根目录；只生成公共引擎，不含任何项目 logic
cmake -S vision_analysis -B build/runtime-manual -DCMAKE_BUILD_TYPE=Release
cmake --build build/runtime-manual --parallel 4

# 手动让某个项目复用这份库
cmake -S projects/person_count -B projects/person_count/build/manual \
  -DRKVISION_ENGINE="$PWD" -DRKVISION_RUNTIME_DIR="$PWD/build/runtime-manual/libs" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build projects/person_count/build/manual --parallel 4
./vision package projects/person_count --build-dir projects/person_count/build/manual
```

不传 `RKVISION_RUNTIME_DIR` 时，直接 CMake 会在当前项目构建目录生成引擎共享库，
不会复用 `build.sh` 的中央缓存；两种方式产物结构相同。

仅修复引擎内部实现、保持 ABI 及运行依赖兼容时，可以停止应用后替换其
`libs/librkvision.so.1` 并重启，业务源码和 assets 无需搬动，应用可执行文件无需重编译。
发布优先重新生成完整包，以同步版本、库校验值和依赖，并使用既有安装回滚流程。
修改 SDK 布局、函数签名或编译环境时要重新构建项目。修改 Web 页面则单独更新 Web，页面不在引擎 `.so` 内。
共享库更新需要重启进程；现有的 JSON 配置热重载仍照常使用。

开发时可从框架根目录执行 `./vision run projects/person_count`（会先增量构建），
或进入发布目录执行 `./run.sh`。先按现场情况设置视频源、模型和硬件参数。

## Web 部署

```bash
cd projects/person_count
./build.sh package
sudo ../../vision install dist/person-count
```

或把 `dist/person-count.tar.gz` 上传 Web「程序管理」，选择配置、检查视频源和模型后启动。
Web 与命令行安装默认保留现场 assets 和 run.config；明确替换时用 `--replace-assets` 或取消 Web
保留选项。`.data/<应用>` 中的持久数据不随更新删除。升级会停止当前视觉应用，需在 Web 重新启动。

不同项目的 ID 独立，上传后分别显示为不同应用。原来已安装的 reference-app 不会自动改名或删除；
可先验证新项目，再通过 Web 删除不再使用的旧应用。

项目初始配置的适用范围见 [项目目录](../../projects/README.md)。性能与验证见
[重构验收记录](../architecture/refactor-validation.md)。
