# RK3588 统一离线环境目录

这个目录同时包含制包脚本、安装入口和全部离线材料。完成制包后，只需把整个
`offline_install_env_debian` 随同制包时的同版本项目复制到新设备，不需要再校验外层压缩包、
解压目录或填写 `--project-root`。

## 目录结构

```text
offline_install_env_debian/
├── create_bundle.sh       # 有网开发机：生成/更新离线软件包
├── install_offline.sh     # 断网现场机：唯一安装入口
├── dependency_manifest.sh # APT、Python requirements、Node 版本清单
├── README.md
└── output/
    └── bundle/            # create_bundle.sh 生成，复制时必须完整保留
        ├── apt/
        ├── python/
        ├── node/
        ├── frontend/
        ├── project/
        ├── BUNDLE_INFO
        └── SHA256SUMS
```

`output/bundle` 是离线安装的实际材料，不是另一个需要手工执行的项目。顶层
`install_offline.sh` 会自动进入该目录，并在修改系统前校验其中的全部 SHA-256、目标
系统版本、ARM64 架构、Python ABI，以及项目依赖和前端源码版本。

## 1. 有网开发机生成材料

制作机必须是 ARM64 RK3588，并与现场机使用相同发行版和版本，例如同为 Debian 11。
在项目根目录运行：

```bash
cd /userdata/rk3588_visual_analysis_framework
bash offline_install_env_debian/create_bundle.sh
```

若现场机还需要从源码编译 C/C++ 程序：

```bash
bash offline_install_env_debian/create_bundle.sh --build
```

脚本只下载 APT/Python/Node/npm 材料，并在临时目录构建前端，不会在制作机执行系统级
`apt install`、`pip install` 或 Node 安装。成功后会原子替换 `output/bundle`；中途失败
不会破坏上一版成品。

制包需要公网或可用的内网镜像。APT 使用制作机的 `/etc/apt/sources.list*`；PyPI/npm
可通过标准环境变量切换镜像，例如：

```bash
PIP_INDEX_URL=https://你的内网PyPI/simple \
NPM_CONFIG_REGISTRY=https://你的内网npm \
bash offline_install_env_debian/create_bundle.sh
```

## 2. 复制到断网设备

必须复制以下两项，并保持相对位置不变：

```text
/userdata/rk3588_visual_analysis_framework/              # 制包时的同版本项目
/userdata/rk3588_visual_analysis_framework/offline_install_env_debian/
```

如果项目本身已经在现场机，请用制包机上的整个 `offline_install_env_debian` **完整替换**现场项目
中的同名目录，不要把新版 `output/bundle` 与旧版合并。也不要只复制
`install_offline.sh` 或遗漏 `output/bundle`。

## 3. 断网设备一条命令安装

root 用户直接运行：

```bash
bash /userdata/rk3588_visual_analysis_framework/offline_install_env_debian/install_offline.sh
```

非 root 用户可加 `sudo`。安装器会自动找到 `output/bundle` 和项目根目录，全程只使用
本地 `file:` APT 仓库与 pip `--no-index`，最后自动执行 `install_deps.sh --check`。
该检查还会核对 RK3588、RKNPU/RGA/MPP 驱动与设备节点、Rockchip GStreamer 插件，
并扫描项目和 `/opt/ai_apps` 中的应用动态库；致命问题会使离线安装返回失败。

如需先做完全只读的检查：

```bash
bash /userdata/rk3588_visual_analysis_framework/offline_install_env_debian/install_offline.sh --verify-only
```

安装 Web 控制台服务时使用：

```bash
sudo env OFFLINE=1 bash /userdata/rk3588_visual_analysis_framework/web_console/install.sh
```

## 4. 新增项目依赖后

- APT 包：加入 `dependency_manifest.sh` 的 `APT_RUNTIME` 或 `APT_BUILD`；
- Python 包：更新对应的生产 `requirements.txt`；新增 requirements 文件时同时登记到
  `PYTHON_REQUIREMENTS`；
- npm 包：在有网开发机更新 `package.json` 和 `package-lock.json`。

每次依赖或前端构建输入变化后，都重新运行 `create_bundle.sh`，然后重新复制整个
`offline_install_env_debian`。APT 通过空 dpkg 状态解析 `Depends/Pre-Depends` 完整闭包，pip
根据 wheel 元数据解析递归依赖，npm 根据锁文件恢复完整依赖树。

Rockchip BSP 中的 RKNPU 内核驱动、RGA、MPP 和厂家 GStreamer 硬件插件不能跨系统
通用，不会从软件站收集；现场机必须保留厂家镜像中的这些组件。项目使用的用户态
`librknnrt.so` 已固定在 `vision_analysis/vendor/rknn/`。

## 5. 常见报错

- `缺少完整离线材料`：没有复制完整目录，或尚未在开发机成功制包；
- `项目依赖/前端构建输入不一致`：现场项目不是制包时的版本，应复制对应项目或重新制包；
- `平台不一致`：制作机与现场机的发行版、版本、架构不一致；
- `npm extraneous/invalid`：重新运行统一离线安装入口恢复锁定依赖，不要在断网机运行
  `npm install` 或 `npm ci`。

包内 `SHA256SUMS` 能发现复制损坏和误混文件；它不替代正式发布签名。
