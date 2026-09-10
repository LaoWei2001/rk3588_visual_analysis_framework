# Ubuntu RK3588 离线安装包

此目录可以随整个项目提前复制到联网 Ubuntu RK3588。它使用 Ubuntu 自己的 APT
索引、当前机器上的 Rockchip BSP 厂商包、ARM64 Python wheels 和项目源码生成离线包。

不能只复制本目录：制包时还需要父目录中的 `vision_analysis/`、`web_console/`、
`service/`、`install_deps.sh` 和共用制包器。制作机和目标机必须是相同 Ubuntu
版本、arm64 架构，最好来自相同的 RK3588 厂商 BSP 镜像。

## 在联网 Ubuntu RK3588 上执行

```bash
cd /userdata/rk3588_visual_analysis_framework

# 首次使用：安装项目依赖和制包工具
bash offline_install_env_ubuntu/prepare_host.sh

# 全量刷新并生成完整开发包
bash offline_install_env_ubuntu/create_bundle.sh --refresh-debs
```

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
