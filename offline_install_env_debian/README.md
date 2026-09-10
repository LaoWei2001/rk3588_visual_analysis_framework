# RK3588 环境与 Web 控制台一键离线安装包

这里生成的是可以在新 RK3588 设备上一键安装的环境仓库。用户只需要运行一次安装脚本；
Web 控制台、systemd 服务、系统 Python 依赖、Rockchip 用户态库，以及默认的项目源码、
C/C++ 编译环境、Node.js/npm 和前端 `node_modules` 都会一起安装，但不会向 Web 程序列表
预装任何 C++ 程序。

## 用户只需要这两条命令

在能够联网、且系统版本与目标机一致的 ARM64 开发机生成安装包：

```bash
cd /userdata/rk3588_visual_analysis_framework
bash offline_install_env_debian/create_bundle.sh
```

把生成的 `output/full-bundle` 目录复制到新设备，然后进入安装包目录安装：

```bash
cd /userdata/full-bundle
sudo bash install_offline.sh
```

安装完成后直接访问：

```text
http://<RK3588-IP>:8080
```

无需再运行 `web_console/install.sh`，也无需手工执行 pip/npm 安装依赖。安装器会在目标机上
实际运行一次前端构建作为验收。安装完成后的程序列表为空；需要运行的程序由用户之后通过
Web 上传，或使用 `vision_analysis/install_app.sh` 明确安装。

## 安装了什么

本地 APT 仓库中的核心软件包是：

- `vision-analysis_*.deb`：Web 控制台前后端和 systemd 服务，不包含默认 C++ 程序；
- `vision-analysis-deps_*.deb`：项目运行依赖元包；
- `vision-analysis-build-deps_*.deb`：默认安装的 C/C++ 与前端开发环境元包；
- `vision-analysis-source_*.deb`：含前端 `node_modules`、可直接修改和编译的完整项目源码；
- `vision-analysis-node-toolchain_*.deb`：固定版本的 ARM64 Node.js/npm；
- `vision-analysis-python-deps_*.deb`：向系统 `/usr/bin/python3` 安装项目 requirements；
- `vision-analysis-rockchip-files_*.deb`：项目固定的 RKNN Runtime 和头文件；
- Debian 官方依赖以及开发机上由 dpkg 管理的必要 Rockchip 厂商包。

完整源码中包含 `first_net_config/`，需要时可在目标机自行编译。bundle 根目录不再额外放置
预编译的 `first_net_config`，避免源码和独立二进制重复交付。

这些内部软件包由安装器统一选择，不需要用户逐个安装；`install_offline.sh` 会一次完成
全部安装。

主要安装路径：

```text
/opt/ai_apps/_console                     Web 控制台
/opt/ai_apps/.data                        运行数据
/usr/bin/python3                          Python 解释器（系统环境）
/usr/local/lib/nodejs/node-v*/            离线 Node.js/npm 工具链
/opt/vision-analysis/rockchip             固定的 Rockchip 用户态文件
/lib/systemd/system/rk3588-console.service Web 控制台服务
/userdata/rk3588_visual_analysis_framework 源码固定入口
```

源码实际保存在带 deb 版本号的目录，例如：

```text
/userdata/rk3588_visual_analysis_framework-2.0.20260908010130
```

固定入口 `/userdata/rk3588_visual_analysis_framework` 是指向最新版的符号链接。在板端修改过的
旧版本源码目录不会在升级时被删除或覆盖；如果这个固定入口原本就是用户自己的真实目录，安装器
也不会覆盖它，而会在安装结束时显示新版源码的实际目录。

完整包安装后可以在目标机编译，编译结果仍需由用户明确上传或安装：

```bash
cd /userdata/rk3588_visual_analysis_framework/vision_analysis
./build.sh dist
sudo ./install_app.sh dist

cd /userdata/rk3588_visual_analysis_framework/web_console/frontend
npm run build
```

在全新设备上，`npm` 会指向随包工具链；如果系统原本已有 Node.js 18+ 和 npm，则保留现有
命令。无论哪种情况，随包工具链的固定入口都保留在 BUNDLE_INFO 记录的目录中。

程序列表中的所有程序均按同一套规则启动、覆盖和删除，没有软件包来源标签或特殊限制。

## 制包过程

`create_bundle.sh` 会自动完成：

1. 在制作机临时编译当前 C++ 主程序，用于检查源码和识别 ELF 依赖；
2. 从新生成的 ELF、脚本和依赖清单检测 APT 依赖；
3. 下载完整的离线 APT 依赖闭包；
4. 下载 Python wheels，并生成安装到系统 Python 的依赖包；
5. 构建 React 前端，并封装 ARM64 Node.js/npm 工具链；
6. 生成不含默认 C++ 程序的 Web 控制台 deb，以及携带 `node_modules` 的源码 deb；
7. 生成依赖元包和本地 APT 索引；
8. 在空 dpkg 状态下验证依赖能够闭合，然后原子替换上一版仓库。

脚本会自动安装 `dpkg-repack` 等制包工具；APT、PyPI 和 npm 源需要可用。已有且与锁文件
一致的 `node_modules` 会直接复用。制包入口不接受模式参数，固定输出：

```text
offline_install_env_debian/output/full-bundle
```

生成失败时不会破坏上一版仓库。`output/`、`frontend/dist/` 和 `node_modules/` 都是生成物，
不需要提交 GitHub；发布时只需压缩或复制 `full-bundle` 目录。

## 升级、修复与卸载

项目代码或依赖变化后，在开发机重新运行 `create_bundle.sh`，再把新版生成目录复制到设备，
进入该目录重新运行 `install_offline.sh`，即可完成升级。安装器会强制重装项目自有环境和
控制台软件包，所以即使目标机误删了控制台文件，也能恢复并重新核验系统 Python 依赖。

升级时会保留：

- `/opt/ai_apps/.data` 中的运行记录和连接数据；
- 用户自行上传或安装到 `/opt/ai_apps` 的程序；
- `/userdata` 下旧版本的源码目录以及其中的板端修改；新版源码会安装到新的版本目录。

Python 依赖直接安装进系统解释器，不再创建 `/opt/vision-analysis/python-env`。pip 会跳过已经
满足 requirements 版本约束的包，只安装缺失项或调整不符合约束的版本；安装完成后会执行
模块导入测试和 `pip check`。由于这些文件不由 deb 逐项拥有，卸载
`vision-analysis-python-deps` 不会自动删除系统 Python 中已安装的模块。

只卸载 Web 控制台：

```bash
sudo apt remove vision-analysis
```

## 新增依赖

大多数依赖不需要手工维护：

- 修改生产 `requirements.txt` 后重新制包，Python 依赖 deb 会更新；
- 修改 `package.json` 或 `package-lock.json` 后重新制包，前端会重建并进入控制台 deb；
- 新增 C/C++ 链接库后直接重新制包，脚本会先构建应用，再从 ELF 的 `NEEDED` 项找到提供
  动态库的 Debian 软件包；
- 新增生产 requirements 文件时，把路径加入 `dependency_manifest.sh` 的
  `PYTHON_REQUIREMENTS`。

正常制包无需单独运行依赖探测器。若排障时需要查看运行依赖识别结果，可以执行：

```bash
bash offline_install_env_debian/detect_apt_dependencies.sh
```

仅头文件依赖、运行时动态加载插件和脚本间接调用的命令不一定能静态识别。此时直接把 APT
包名逐行写入对应文件：

```text
offline_install_env_debian/extra-runtime-packages.txt
offline_install_env_debian/extra-build-packages.txt
```

保存后仍然只需重新运行 `bash offline_install_env_debian/create_bundle.sh`。

## “空白 Debian”的边界

这里的空白系统是指：RK3588 已经能使用正确的厂家内核、设备树和固件启动，但 Debian
用户态尚未安装本项目。安装包会带完整 Debian 用户态依赖、必要的 RGA/MPP/Rockchip
GStreamer 厂商包和项目固定的 RKNN Runtime。

内核驱动、设备树、固件和 `/dev/rknpu`、`/dev/dri` 等设备节点无法由普通用户态 deb 从
另一台机器安全迁移，仍需来自适配板卡的 BSP/系统镜像。未由 dpkg 管理的固定 `.so` 如需
携带，应明确加入 `dependency_manifest.sh` 的 `BUNDLED_RUNTIME_FILES`。

安装完成后可按需执行 `bash install_deps.sh --check` 诊断硬件、驱动和动态库。它是排障工具，
不是一键安装流程的一部分，也不会替代真实摄像头和 NPU 推理测试。
