# `src/core`：全局控制块与公共运行时能力

## 文件职责

- `app_ctrl.h/.cpp`：`g_pCtrl`、配置和通道状态、初始化/释放、线程安全查询、配置监控和热切流。
- `constants.h`：最大通道数等公共常量。
- `pause_ctrl.h/.cpp`：全局暂停/恢复、空格键和 `SIGUSR1` 协作。
- `image_utils.h/.cpp`：图片编码/落盘等复用工具。

当前源码没有 `base64_util`，二次开发不要引用旧文档中的该文件。

## `APP_CTRL`

`APP_CTRL` 是固定容量、pthread 风格的进程级控制块，不是动态 `vector<unique_ptr<...>>` 容器。它包含：`AppConfig`、拼接显示缓冲、`DecChannel* capturers[]`、`ChannelState channels_state[]`、配置 rwlock、每通道 mutex、条件变量、运行标志和监控线程句柄。全局指针是 `g_pCtrl`。

`app_ctrl_init(const char *cfgPath)` 分配并初始化控制块、加载配置、初始化锁和通道初始状态；配置监控线程由 `main()` 创建，不是在此函数内部启动。`app_ctrl_deinit()` 必须在线程和依赖模块停止后调用。

## `ChannelState` 所有权

| 字段组 | 主要写方 | 访问约束 |
|---|---|---|
| `tile_staging`、显示 FPS 计数 | display worker | worker 独占；跨线程读取 FPS 走 API/锁 |
| `last_infer_ts_ms` | frame inlet | 健康检查按现有接口读取 |
| 在线状态、结果、ROI、draw、logic state、匹配帧和序号、运行时推理开关 | analyzer/control/config reload | 由 `chn_mtx[chnId]` 保护 |

不要长时间持有 `chn_mtx` 做 OpenCV、推理、网络或磁盘工作。惯用模式是锁内复制必要值，锁外计算，再锁内一次性提交。

## 同帧快照

`app_ctrl_get_channel_snapshot()` 在一把通道锁内复制 `last_logic_frame`、`last_results`、logic state、FPS、在线状态、帧序号和结果年龄。`ChannelContext::get_channel_snapshot()` 与 `GlobalContext::get_channel_snapshot()` 是业务层包装。

快照的 frame/results 同帧；普通实时显示则允许“最新画面 + 最近结果”。跨通道逻辑需要同帧语义时只能用快照，不要分别调用多个 getter 拼接。

其他安全查询包括结果/新鲜结果、目标数量、display/infer FPS、最后推理时间和 logic 名称。调用前仍应检查通道范围和返回值；新鲜结果 API 的年龄阈值用于避免断流后使用旧检测。

## 热重载

`config_monitor_thread_func()` 在 core 中导出，由 `main()` 创建。它负责模型 reload、字段同步、ROI 重建、logic 状态切换、global logic 重启、阈值/tracker 更新和 stream 重建。拓扑不变是硬约束，详情见 [config.md](config.md)。

## 暂停

`pause_ctrl` 用于整条处理链的协作暂停。新增阻塞线程时应确认暂停和退出都能唤醒它；信号退出路径会调用 `resume_all()`，避免线程停在暂停条件上导致 join 卡死。
