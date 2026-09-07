# RK3588 一键离线依赖仓库

目标只有一个：在有网 ARM64 开发机生成项目需要的 `.deb`，复制到新设备后一条命令安装。
离线包不再绑定项目源码哈希，不要求现场项目与制包时逐文件一致，也不会把硬件诊断当成安装失败。

## 目录和产物

```text
offline_install_env_debian/
├── create_bundle.sh              # 开发机生成新版仓库
├── detect_apt_dependencies.sh    # 查看当前能自动识别的 APT 依赖
├── install_offline.sh            # 新设备唯一安装入口
├── dependency_manifest.sh        # 基础依赖及 requirements 位置
├── extra-runtime-packages.txt    # 手工补充的运行依赖
├── extra-build-packages.txt      # 手工补充的编译依赖
└── output/bundle/apt/            # 生成的本地 APT 仓库
```

仓库中除 Debian/Ubuntu 原始软件包外，还会自动生成：

- `vision-analysis-deps_*.deb`：项目运行环境元包；
- `vision-analysis-python-deps_*.deb`：由生产 requirements 生成的隔离 Python 环境；
- `vision-analysis-frontend-assets_*.deb`：预构建的前端 `dist`；
- `vision-analysis-rockchip-files_*.deb`：项目固定的 RKNN Runtime 和头文件；
- 默认生成 `vision-analysis-build-deps_*.deb`，让新设备能够直接编译项目。

`dist` 和 `node_modules` 都不需要提交 Git。制包脚本在开发机按锁文件准备前端依赖
（已有完整 `node_modules` 时会直接复用）并执行 `npm run build`，只把最终 `dist`
放入前端资源 deb；新设备不安装 Node/npm。

## 生成离线仓库

在有网 ARM64 开发机运行：

```bash
cd /userdata/rk3588_visual_analysis_framework
bash offline_install_env_debian/create_bundle.sh
```

制作机需要已经安装 `dpkg-repack`，并且当前 APT 软件源可正常下载与目标 Debian
版本相同的 arm64 软件包。Debian 11 安全更新使用 `dependency_manifest.sh` 中固定的 Debian
官方 snapshot 日期，避免发行版归档时出现索引存在但 deb 已从镜像移走的问题。脚本会按
“空 dpkg 状态”解析完整依赖闭包：Debian 官方包一律下载统一索引的候选版本；只有软件源中
不存在、但开发机已安装的瑞芯微厂商包才会重新封装。
这样不会再把开发机上的旧版 `systemd` 等官方包与新版软件源混装。

默认包含运行环境和 C/C++ 编译环境。只有明确不需要在目标机编译时才使用精简模式：

```bash
bash offline_install_env_debian/create_bundle.sh --runtime-only
```

成功产物固定在 `offline_install_env_debian/output/bundle`。生成过程使用临时目录，失败不会
破坏上一版仓库。

## 新设备一键安装

把整个 `offline_install_env_debian` 复制到新设备，然后运行：

```bash
sudo bash offline_install_env_debian/install_offline.sh
```

默认命令已经安装编译环境。只有仓库使用 `--runtime-only` 制作时，安装才相应使用：

```bash
sudo bash offline_install_env_debian/install_offline.sh --runtime-only
```

安装器只使用包内 `file:` APT 仓库。架构不一致时会停止，因为 arm64 deb 无法安装到其他
架构；发行版版本不一致只警告，最终由 APT 判断包是否兼容。

Python 依赖安装到 `/opt/vision-analysis/python-env`，不会污染系统 Python。前端产物安装到
`/usr/share/vision-analysis/frontend/dist`。项目固定的 RKNN Runtime 安装到
`/opt/vision-analysis/rockchip/lib`。之后安装 Web 控制台：

```bash
sudo bash web_console/install.sh offline
```

## 新增依赖

### Python 或前端依赖

- 修改生产 `requirements.txt` 后直接重新运行 `create_bundle.sh`；Python 依赖 deb 会自动更新。
- 修改 `package.json`/`package-lock.json` 后直接重新制包；前端资源 deb 会自动更新。
- 新增一个生产 requirements 文件时，把路径加入 `dependency_manifest.sh` 的
  `PYTHON_REQUIREMENTS`。

### C/C++ 动态库

新增库后先正常构建一次，再运行 `create_bundle.sh`。检测器会读取 ELF 的直接 `NEEDED`
动态库，通过开发机 dpkg 数据库找到提供它们的软件包，并生成新版依赖元包。

可单独查看检测结果：

```bash
bash offline_install_env_debian/detect_apt_dependencies.sh
bash offline_install_env_debian/detect_apt_dependencies.sh --build
```

### 无法自动判断的依赖

脚本中调用的外部命令、仅头文件依赖或运行时动态加载插件不一定能静态识别。可以一条命令添加：

```bash
bash offline_install_env_debian/create_bundle.sh --add jq
bash offline_install_env_debian/create_bundle.sh --add-build libexample-dev
```

成功制包后包名会分别记入 `extra-runtime-packages.txt` 或 `extra-build-packages.txt`，以后自动携带。

## “空白 Debian”的边界

这里的空白系统是指：RK3588 已经能用正确的厂家内核、设备树和固件启动，但 Debian 用户态
尚未安装项目依赖。仓库会带上完整 Debian 依赖闭包、开发机上由 dpkg 管理的 RGA/MPP/
Rockchip GStreamer 用户态包，以及项目固定的 RKNN Runtime。

内核驱动、设备树、固件和 `/dev/rknpu`、`/dev/dri` 等硬件节点不能由普通用户态 deb 从另一台
机器安全迁移，仍必须来自适配该板卡的 BSP/系统镜像。未登记到 dpkg、来源和版本无法确认的
散落 `.so` 也不会被盲目复制；需要携带这类固定文件时，应明确加入
`dependency_manifest.sh` 的 `BUNDLED_RUNTIME_FILES`。硬件项可另外运行
`bash install_deps.sh --check` 诊断，但不阻塞离线依赖安装。
