# 视觉项目

所有项目直接平铺在 `projects/` 下，不再按课程等类别嵌套。一个目录对应一个可独立构建、打包和部署的应用，各项目共用仓库中的引擎与 SDK。

| 项目 | 内容 | 初始配置 |
|---|---|---|
| `crane_safety` | 行车运动、吊钩、安全帽、人员入侵及全局联动 | 通道与硬件输出默认停用，需现场配置 |
| `person_count` | ROI 人数统计、跨通道人数聚合 | 原 config_global.json |
| `object_detection` | 基础检测，保留分割与姿态模型 | 原 config_test.json |
| `course_01` … `course_10` | 各课程独立项目 | 各自 assets/config.json |
| `course_gpio` | 独立 GPIO 课程项目 | 默认停用，配置引脚、电平后启用 |
| `dify` | 周期截图与 Dify 上报 | 需设置视频源、上报策略与连接 |
| `relay_control` | 继电器业务 | 默认停用，需配置引脚、电平和视频源 |

例如，从仓库根目录执行：

```bash
./rkvision build person_count
./rkvision package person_count
```

产物位于本项目 `build/` 和 `dist/`，发布包可上传 Web。每个项目的 `CMakeLists.txt` 是真正的
CMake 根工程，以 `add_subdirectory` 引入共用引擎，并只编译自己的业务模块。
一个完整项目可以包含多个通道逻辑、全局逻辑及配置；不需要为每个 logic 建一个项目。

新建项目：从仓库根目录执行 `./rkvision create projects/my_app`，随后运行
`./rkvision build my_app`。仓库外项目使用 `/path/to/framework/rkvision build .`。
项目中的 logic/assets 不需要复制回引擎目录。共享模型在需要它的项目内各自保存，以便独立交付。
原 19 个通道模块、2 个全局模块及全部资源已迁移，所有权记录见 MIGRATION.json。

公共实现编译为 `librkvision.so.1`，各项目可执行程序只包含自身业务、入口和参数清单。
`rkvision` 自动复用框架 `build/runtime/` 的引擎缓存；发布包始终包含 `libs/librkvision.so.1`。
编译、运行、部署和引擎升级详见[独立项目与共享引擎](../docs/development/independent-projects.md)。
