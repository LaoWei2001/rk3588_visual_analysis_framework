# Ubuntu RK3588 离线安装包

此目录可以随整个项目提前复制到联网 Ubuntu RK3588。它使用 Ubuntu 自己的 APT
索引、当前机器上的 Rockchip BSP 厂商包、ARM64 Python wheels 和项目源码生成离线包。

Ubuntu 离线功能已经和 Debian 离线功能完全分离：本目录自带制包器、依赖探测器和
安装器模板，不会调用 `offline_install_env_debian/`。制包仍需父目录中的项目源码，
因此应复制整个项目到制作机；可以删除其中的 `offline_install_env_debian/`，不会影响
Ubuntu 制包。制作机和目标机必须是相同 Ubuntu 版本、arm64 架构，最好来自相同的
RK3588 厂商 BSP 镜像。

本目录中的脚本分工如下。正常使用只运行 `create_bundle.sh`；其余脚本都是内部组件：

- `prepare_host.sh`：在联网 Ubuntu 制作机安装制包工具和项目依赖；
- `detect_apt_dependencies.sh`：独立识别 Ubuntu 运行与编译依赖；
- `create_bundle.sh`：收集 deb、Python、Node.js、Web 控制台和源码；
- `templates/install_offline.sh`：Ubuntu 离线安装器模板，制包时会复制到最终包根目录。

## 在联网 Ubuntu RK3588 上执行

```bash
cd /userdata/rk3588_visual_analysis_framework

# 一条命令自动准备制作机并生成完整开发包
bash offline_install_env_ubuntu/create_bundle.sh
```

`create_bundle.sh` 默认会先调用本目录自己的 `prepare_host.sh`，安装并检查项目依赖和
制包工具，然后临时重新编译主程序；编译失败时不会生成包。它会忽略项目中可能从
Debian 一起复制过来的旧 `vision_analysis` 二进制。

制包入口不接受模式参数，始终刷新依赖状态并生成包含运行环境、编译环境、源码、Node.js
和前端依赖的 `full-bundle`。上一版中版本完全相同的 deb 会安全复用，版本变化的包会自动
重新下载。
如果厂商 `librga-dev` 已提供头文件和 `librga.so`、但遗漏 `librga.pc`，联网安装器会在
核验这些文件后生成兼容元数据；制包器会把它一并带到离线设备。
联网安装器会同时安装 `APT_*` 与 `LOCAL_*` 清单；因此已配置 Rockchip Multimedia PPA
时，缺失的 `librga-dev`、MPP 和 RGA 用户态包也会被补装，而不再只检查发行版官方包。

输出位置：

```text
offline_install_env_ubuntu/output/full-bundle
```

生成后核对元数据：

```bash
sed -n '1,30p' offline_install_env_ubuntu/output/full-bundle/BUNDLE_INFO
```

应当看到 `os_id=ubuntu`、正确的 `os_version_id`、`deb_arch=arm64` 和
`strict_target_os=true`。安装器会拒绝安装到不同 Ubuntu 版本或 Debian。

将 `full-bundle` 复制到相同系统版本的离线 Ubuntu RK3588 后执行：

```bash
cd /userdata/full-bundle
sudo bash install_offline.sh
```

如制作机缺少 `librga2`、MPP 或 Rockchip GStreamer 等厂商包，制包器会明确报错。
这类用户态包应从对应 Ubuntu BSP 镜像或板卡厂商仓库安装，不能拿 Debian 版本替代。

少数无法自动识别的依赖，直接把 APT 包名逐行写入 `extra-runtime-packages.txt` 或
`extra-build-packages.txt`，然后重新运行同一条 `create_bundle.sh` 命令。
