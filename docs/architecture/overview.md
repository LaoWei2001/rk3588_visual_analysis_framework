# 代码职责与依赖边界

本文是源码导航和维护约定。引擎内部仍存在共享运行状态；目录分组不代表所有模块已经实现依赖隔离。

## 一眼定位修改位置

| 要修改的能力 | 主要目录 | 责任边界 |
| --- | --- | --- |
| 主程序启动、停止、配置热更 | `engine/src/application/` | 编排组件生命周期，不实现模型后处理或业务告警规则 |
| 每帧调度、显示线程、结果发布 | `engine/src/pipeline/` | 维护队列和线程处理逻辑，协调媒体、推理与业务；线程创建与回收由 application 编排 |
| 解码、渲染、录像、RTSP | `engine/src/media/` | 提供媒体能力；显示线程在 `pipeline/display_worker.*`，渲染接口在 `media/display/display_render.h` |
| 模型与推理调度 | `engine/src/vision/` | `inference` 管任务与 NPU；`models` 根目录放共享模型接口，子目录放具体适配器 |
| 事件媒体与落盘 | `engine/src/events/` | 生成本地事件和媒体；远端投递由独立 Python 服务负责 |
| 业务规则 | `projects/modules/`、`projects/global_modules/` | 通过公开 SDK 处理通道/跨通道业务，配置声明在各模块 `logic.json` |
| 公共 C++ 接口 | `engine/include/` | 业务可用 API；不暴露引擎内部运行状态与厂商 SDK |
| 页面和交互 | `web_console/frontend/src/pages/`、`components/` | 组合 UI；共享模型、几何和校验放 `models/` |
| 前端请求与状态 | `web_console/frontend/src/api/`、`store/` | API 负责传输，store 负责会话状态，纯模型不导入两者或组件 |
| HTTP 请求与鉴权 | `web_console/backend/routers/`、`dependencies.py` | 解析请求、权限判断、错误映射；避免路由之间导入业务函数 |
| Web 应用服务 | `web_console/backend/services/` | 设备、媒体、配置等操作；阻塞调用在线程池执行 |
| 安装、进程与恢复 | `services/framework/runtime_manager/python/rk3588_runtime/` | CLI 和 Web 共用的生命周期与运行锁，不依赖 FastAPI |
| 远端投递与模型更新 | `services/framework/upload/`、`model_update/` | 各自独立进程，消费事件/更新配置，不在 C++ 回调内联网 |
| 构建、板端诊断、离线交付 | `tools/build/`、`tools/device/`、`tools/offline_dev_install/` | 工具各自独立入口；源码生成器与构建产物目录分开 |

## 必须只有一个实现的机制

- **安装事务**：`rk3588_runtime/package_install.py` 校验包、准备目录、停止旧进程、替换与回滚。
  Web `routers/apps.py` 负责上传解包和 HTTP 适配，`install_app.sh` 负责命令行交互。
  包校验与准备失败不得先停止已运行程序。删除应用也必须在同一运行锁内完成停止、删除和运行意图清理。
- **应用目录边界**：Web 侧复用 `services/app_paths.py`。纯路径解析与 HTTP 异常映射分开，
  不在每个路由复制 `APPS_ROOT / name` 加 `exists()` 的检查。
- **系统管理员鉴权**：`backend/dependencies.py` 的 `require_system_admin`，供网络、相机、恢复和系统路由共用。
- **数据集模型**：`frontend/src/models/dataset.ts` 持有类型与兼容转换；store 不从 API 或组件导入纯模型。
- **推理 ROI 几何**：`frontend/src/models/inferenceRoi.ts` 提供校验和坐标算法，UI 只使用它们。
- **帧抓取**：`backend/services/frame_snapshot.py` 管 OpenCV 资源与编码，`routers/snapshot.py` 在线程池调用并映射 HTTP 错误。
- **模型公共能力**：`vision/models/model_base.h`、`model_group.*`、`rknn_output_buffers.h` 等放在模型根目录。
  YOLO 专用张量约束仍归 `models/yolo/`，不以“通用工具”为名混入公共接口。
- **模型创建与单帧执行**：视频启动、模型热更和图片会话共用 `models/model_registry.cpp` 的
  `create_model_group()`。`ModelGroup` 保存模型和合并结果，不继承单个模型接口；实际推理统一由
  `inference/frame_executor.cpp` 执行。视频调用方明确传入跟踪所需的解码阈值，图片调用方传入用户配置值。
- **业务上下文工具**：`logic/channel_context.cpp` 集中图片访问、参数读取和变量发布，
  `logic/target_query.cpp` 集中目标与区域查询。跨通道取图依赖正式运行状态，实现放在
  `application/runtime/channel_snapshot.cpp`；通用蒙版绘图放在 `media/display/mask_blend.cpp`。

后端的 runtime 模块别名桥接、前端 API 的兼容类型导出承担导入兼容职责，不是第二套实现。
删除前必须核对部署环境和外部调用方。教学示例的相似代码也不等同于可删的生产冗余。

## 命名约定

| 对象 | 约定与示例 |
| --- | --- |
| C++ / Python 自有源文件 | `snake_case`，例如 `decode_channel.cpp`、`frame_snapshot.py` |
| React 组件和页面 | `PascalCase.tsx`，例如 `DatasetTaskEditor.tsx` |
| TypeScript 模型、API、store | 小写开头，按职责命名，例如 `models/inferenceRoi.ts`、`api/dataset.ts` |
| 自有目录 | 小写，按能力命名；避免新增泛化的 `misc`、`helpers2` 或不明用途的 `manager` 目录 |
| CMake 模块 | 按构建职责命名，使用既有 `Engine.cmake`、`Business.cmake` 等形式 |
| 第三方和外部契约 | 保留上游名称、配置 ID、HTTP 路径；例如既有 `zip-depth` 适配目录、`zip_depth` 模型 ID |

已有公开 API、类名和配置字段的改名属于兼容性变更，不能仅为统一外观批量替换。
新增文件先判断已有职责目录能否容纳；不要只为缩短文件行数切出仍依赖全部共享状态的碎片文件。

## 尚待处理的结构问题

以下问题在审核中确认，不能把当前整理描述为“已完全消除耦合和重复”：

| 位置 | 问题 | 后续改造边界 |
| --- | --- | --- |
| `application/runtime/app_ctrl.*` | 全局运行对象连接管线、媒体、推理和业务，仍有跨层依赖 | 按组件收窄状态与访问接口，保留快照和退出顺序，需并发/热更/退出验证 |
| `application/control/remote_dataset.cpp` | 同时包含 Socket、任务状态、规则、编码和缓存；由 Logic 控制服务间接启动 | 采集会话归 `dataset`，控制层只协议适配，生命周期改为显式编排 |
| `application/control/logic_control.cpp`、`file_playback_control.cpp` | 重复 Socket 收发代码，单次 `recv` 对分片请求不可靠；dataset 的实现又不同 | 先明确消息边界、大小与超时，再共享传输层，覆盖分片、取消及旧客户端兼容 |
| `projects/modules/logic_crane_*/detection_utils.h` | 多份检测辅助函数；helmet 与 intrusion 的非法置信度过滤已不一致 | 建立业务共享支撑，明确统一校验规则，避免把工业业务语义放进引擎 SDK |
| `logic_crane_hook_calibration/logic.cpp` | 直接依赖另一个 Logic 模块的 `hook_geometry` | 提取共同标定几何，验证单独移除/构建各模块 |
| Debian / Ubuntu 离线制包脚本 | 依赖探测和制包流程重复，部分安装格式校验已分叉 | 共享制包机制，发行版清单分开，分别验证完整离线安装 |
| `EditorPage.tsx`、`LiveViewPage.tsx` | 文件管理、图编辑、媒体会话等多组状态仍集中在页面 | 按状态所有权拆 hook/组件，验证保存、撤销和播放会话边界 |

抓帧移到线程池只隔离了事件循环；底层驱动/网络读取卡死仍需要超时、并发限制或独立进程治理。
同样，源码目录整理不等于硬件稳定性、关机取消或现场性能已经得到验证。

C++ 文件入口见[引擎导航](../../engine/README.md)。
