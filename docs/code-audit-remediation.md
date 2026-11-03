# 项目审核整改记录

## 第一批：可靠性与重复实现

本批基于当前工作区增量修改，保留此前未提交的功能变更；不安装、不重启运行中的服务。

### 已实施

- 配置读取、保存、删除共用路径解析；拒绝相邻目录前缀绕过、外部文件软链接和 assets 目录软链接越界。
- 资源和配置路由复用应用目录校验；Action 路由也使用同一边界。
- 上传使用每请求独立临时文件；不覆盖模式使用原子硬链接发布，防止跨线程/进程同名竞争；失败清理临时文件并保留旧资源。
- 通道/全局 Action 共用 Socket 客户端，在线程池执行，支持分片响应、总超时和响应长度限制。
- 批量告警删除移到线程池；配置写入也在线程池执行。
- 全局移除 `-ffast-math`，恢复 NaN/Inf 检查语义；新增无需 NPU 的元数据和浮点校验测试。
- YOLOv5、YOLOv8检测、YOLOv5分割：查询结构初始化、返回值检查、输入/输出基本契约校验、构造异常资源释放。
- 三个前端页面/组件复用公共错误转换；公共转换支持 FastAPI 校验错误数组。
- 后端模型类型列表复用模型检查模块定义。
- 存储维护循环从路由移入服务层，启动入口显式依赖该服务。
- 111个嵌套构建文件和一个首次联网工具二进制取消 Git 跟踪，本地文件保留；增加精确忽略规则。

### Action 接口兼容性

原有文档明确规定外部控制器可免登录提交 Action，因此本批提供可选加固而非直接切断现有集成。

- 未设置 `LOGIC_ACTION_TOKEN`：保留兼容免登录模式，启动时提示。
- 设置非空 `LOGIC_ACTION_TOKEN`：调用方必须提供正确的 `X-Logic-Token`，或者有效的控制台 Bearer 登录。
- 令牌不通过 URL 传输、不打印其值；目前没有逐应用/逐动作的细粒度授权。
- 本批没有修改设备服务的实际环境变量，因此不能声称现网已经启用鉴权。

### 验证命令

```bash
PYTHONPATH=web_console/backend python3 -m pytest -q \
  web_console/backend/tests/test_audit_regressions.py \
  web_console/backend/tests/test_config_io.py \
  web_console/backend/tests/test_assets.py \
  web_console/backend/tests/test_logic_control.py \
  web_console/backend/tests/test_record_export.py \
  web_console/backend/tests/test_model_info.py \
  web_console/backend/tests/test_config_sync_reads.py \
  web_console/backend/tests/test_records_split_event.py \
  web_console/backend/tests/test_alarm_retention.py

cmake -S engine -B build/engine -DBUILD_MODEL_METADATA_TESTS=ON
cmake --build build/engine -j4
# 在 build/engine 中执行；设备旧版 ctest 不支持 --test-dir。
cd build/engine
ctest --output-on-failure -E '^zip_depth_runtime_regression$'
```

前端在 `web_console/frontend` 中执行 `npm run build`。
硬件 ZipDepth 测试未纳入这批自动执行，避免与已运行的设备任务竞争 NPU。

已验证：上述后端用例共78项通过；C++主程序全量构建通过，23项非硬件CTest通过；
前端TypeScript检查及Vite生产构建通过。该批仍有单个JS包过大的提示，已在第二批继续处理。
实时日志页面7项浏览器回归通过；测试桩补齐了现有页面依赖的 `/api/auth/me`。

## 第二批：代码组织与加载边界

### 已实施

- 原1077行的 `api/client.ts` 拆分为13个业务模块，兼容入口缩至15行；156个函数/类型导出保持一致。
- 视频采集使用公共Axios实例，删除重复鉴权拦截器和错误转换；回归覆盖全部采集请求路径、Bearer请求头、旧401响应与新登录的竞争，以及读取合并。
- 22个终端文件迁入 `src/features/terminal/`；更新源码测试路径和字体glob路径，保留连接、字体及显示功能。
- 终端退出清理使用轻量注册入口，避免其他页面提前加载xterm；页面按路由懒加载。
- 生产构建的入口JS由约1240KB降至256.88KB（gzip约379KB降至87.45KB）；编辑器和终端独立包分别约354KB、407KB，不再触发500KB警告。这是入口资源大小，不是总资源体积或视频推理帧率的测量。
- 前端模型类型后备列表、标签必填和深度模型能力集中到 `src/models/capabilities.ts`；在线可用类型仍以后端为准。
- 测试构建配置统一放在 `engine/CMakeLists.txt`，按职责分组；原有选项与目标名称保留，默认不启用。
- 新增独立 `BUILD_NPU_TESTS`；旧 `BUILD_ZIP_DEPTH_TESTS` 继续兼容启用通用测试。实际模型推理CTest标记为 `hardware`，非硬件回归用 `-LE hardware` 排除。
- 新增 `tools/check.py` 代表性回归入口和API/终端目录说明；运行入口不会安装、部署或重启服务。

### 复验入口

```bash
# 默认：后端关键回归、前端构建与传输/算法、C++构建与非硬件CTest
python3 tools/check.py
# 浏览器：终端连接/显示/反射、实时日志、配置同步；先自动构建前端
python3 tools/check.py --suite browser
# 只列出将执行的命令
python3 tools/check.py --suite backend frontend cpp browser --dry-run
```

依赖需预先安装；浏览器使用Playwright和Chromium，可通过 `CHROMIUM_PATH` 指定路径。
这套入口是本次整改的代表性回归集，不是全仓库所有测试。

### 本批验证结果

- 后端关键回归78项通过；C++主程序构建通过，现有构建目录的23项非硬件CTest通过。
- 新建独立构建目录，开启 `BUILD_NPU_TESTS` 并关闭 `BUILD_ZIP_DEPTH_TESTS`，10项通用NPU测试通过，确认不依赖ZipDepth测试开关。
- 前端TypeScript检查与生产构建通过；API、终端、日志及配置同步回归分组执行，共72项通过。
- 原生全屏1项因无头Chromium环境跳过，需 `CHROMIUM_HEADED=1` 和图形桌面另行验证。
- 配置同步测试桩补齐既有页面依赖的 `/api/auth/me`，31项配置同步回归全部通过；新增退出登录/再次登录测试确认懒加载终端正确释放并创建新会话。
- 156个API函数/类型导出比对一致；迁移后的旧终端源码路径引用与差异空白检查通过。

以上没有运行实际模型的硬件推理CTest，也没有部署或重启设备服务。

## 后续结构整改

以下审核建议尚未完成，不属于本批已交付内容：

- 全部模型的完整张量契约与 RKNN/RGA 资源 RAII 统一；本批只补强旧模型的关键失败路径。
- 前后端/C++统一的能力描述与契约生成；目前后端和前端各自集中定义，尚未跨语言生成。
- EditorPage、LiveViewPage、事件持久化和热更新模块按职责拆分。
- 后端运行库兼容导入逐步显式化。
- CMake依赖发现进一步模块化；硬件视频/NPU稳态性能验证仍需单独安排。
- Action逐应用/逐动作授权与现有控制器迁移。

移除快速浮点优化后的模型数值、视频吞吐和多路稳态性能仍需设备对照测试，不能仅凭构建和 CPU 单元测试宣称无性能变化。
