# 贡献指南

## 修改放在哪里

- 通道和全局业务分别位于 `projects/modules/`、`projects/global_modules/`。
- C++ 框架内部实现按[引擎源码导航](engine/README.md)组织，公开接口位于 `engine/include/`。
- Web API 和服务编排位于 `web_console/backend/`，页面位于 `web_console/frontend/src/`。
- 独立后台服务位于 `services/framework/`，开发和交付工具位于 `tools/`。

修改配置、公开接口、事件格式或目录时，同步更新相关文档、调用方和构建定义。
保留第三方源码的版权声明与版本清单。运行时模型、视频和本机配置不要混入代码整理提交。

## 本地验证

在已安装匹配依赖的 RK3588 开发环境中，从仓库根目录运行：

```bash
python3 tools/build/generate_logics_catalog.py --check
python3 tools/check.py --suite backend cpp --jobs 4
```

`tools/check.py --suite cpp` 编译主程序并运行代表性 C++ 回归，排除 `hardware` 标签。
更多可选测试见 `engine/cmake/Tests.cmake`；真实流、NPU、GPIO 和故障恢复需在相应设备上单独验证。
执行硬件回归前确认它不会与已运行应用争用设备。

前端常规构建可运行 `cd web_console/frontend && npm run build`。如果要完整保留仓库中的
`assets` 目录，将构建结果写到仓库外：

```bash
cd web_console/frontend
./node_modules/.bin/tsc --noEmit --incremental false
./node_modules/.bin/vite build --outDir /tmp/rk-console-build
```

依赖必须预先安装。验证记录应说明实际运行的命令与结果；未执行的硬件验证请明确列出。
性能改动需记录输入、模型、板卡、运行条件，并逐路比较推理、发布和延迟。

## 格式与工作区清理

C/C++ 格式由根目录 `.clang-format` 定义。仅格式化本次修改的文件，避免无关的全仓库格式变更：

```bash
python3 tools/format_cpp.py --check engine/src/application/main.cpp
python3 tools/format_cpp.py engine/src/application/main.cpp
```

格式化工具的默认范围是引擎源码、公开头文件、已有测试和业务模块，排除第三方与生成目录。

```bash
python3 tools/clean.py --verbose        # 预览日志和缓存清理
python3 tools/clean.py --apply          # 执行日志和缓存清理
python3 tools/clean.py --build          # 预览构建/发布产物清理
python3 tools/clean.py --build --apply  # 执行构建/发布产物清理
```

清理工具跳过所有名为 `assets` 的目录、Git 元数据、依赖目录和符号链接；有 Git 时保留已跟踪文件。
含有 `assets` 的生成目录会保留该子目录及其父目录，其他生成文件仍会删除。
离线安装包不在构建清理范围内。`tools/build/` 是源码目录，不会作为构建产物删除。

## 提交改动

PR 说明问题、修改后的行为和验证结果。报告问题时提供板卡、系统/BSP、RKNN 版本、
复现步骤以及脱敏配置和必要日志。源码提交不应包含运行日志、临时复现脚本、编译结果或本机密钥；
可复用的测试应放在所属组件的现有 `tests/` 中。
