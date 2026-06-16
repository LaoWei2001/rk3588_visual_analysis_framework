# 核心控制模块 (core/)

> 应用全局生命周期管理、通道状态维护、配置热监控线程、图像格式转换、Base64 编码工具。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `app_ctrl.h` | 全局控制块 `APP_CTRL`、`ChannelState`、`ChannelSnapshot` 结构定义；所有线程安全查询 API 声明 |
| `app_ctrl.cpp` | `app_ctrl_init/deinit`、配置热监控线程、通道数据读写实现 |
| `constants.h` | 系统级常量（最大通道数、队列大小等） |
| `image_utils.h` | 原始帧（NV12/NV21/RGB/BGR）→ BGR `cv::Mat` 接口 |
| `image_utils.cpp` | OpenCV `cvtColor` 路径实现 |
| `base64_util.h` | Base64 编码接口 |
| `base64_util.cpp` | Base64 编码实现（告警上传时用于图像序列化） |
| `pause_ctrl.h` | 暂停控制接口（选中显示窗口按空格暂停/继续，需开启 `enable_pause_key`） |
| `pause_ctrl.cpp` | 暂停状态维护与各流水线的暂停/继续协调实现 |

---

## 核心数据结构

### APP_CTRL — 全局控制块

整个应用唯一的全局单例，通过 `extern APP_CTRL *g_pCtrl` 在所有子模块中共享：

```
APP_CTRL
  ├── AppConfig config                           运行时配置（与 config.json 同步）
  ├── Display_t dispDesc                         显示区域描述符
  ├── char** pDispBuffer                         GTK 显示缓冲区指针
  ├── vector<unique_ptr<DecChannel>> capturers   采集器实例（按通道索引）
  ├── int inputW / inputH                        模型输入尺寸（如 640×640）
  ├── vector<ChannelState> channels_state[32]    每通道运行时状态
  ├── shared_timed_mutex mtx                     保护 config 读写（读写锁）
  ├── mutex chn_mtx[32]                          每通道独立互斥锁
  ├── condition_variable_any cv_config           热重载/退出通知
  ├── atomic<bool> isRunning                     全局运行标志
  └── unique_ptr<thread> configMonitorThread     配置监控线程
```

### ChannelState — 通道运行时状态

按所有权分为三组，**严格遵守各组的锁约定**，否则会产生数据竞争：

| 组别 | 字段 | 访问规则 |
|------|------|----------|
| **(A) display_worker 独占** | `tile_staging`、`disp_fps`、`fps_counter`、`last_fps_ts` | 仅 display_worker 线程写入，其他线程不得碰 |
| **(B) chn_mtx[chnId] 保护** | `last_results`、`last_result_ts`、`last_frame`、`last_logic_frame`、`roi_zones`(各区域名字+模型坐标多边形)、`roi_zones_raw`(旧像素格式原始顶点)、`draw_cmds`、`logic_name`、`logic_state`、`logic_frame_id`、`last_logic_ts` | 多线程读写均须持 `chn_mtx[chnId]` |
| **(C) 单线程 videoOutHandle** | `last_infer_ts` | 仅解码回调线程中读写，无需加锁 |

### ChannelSnapshot — 原子帧快照

用于需要"图像 + 检测框严格同源"的场景（如 global_logic 上报告警）：

```cpp
struct ChannelSnapshot {
    cv::Mat                 frame;          // BGR，与 results 同帧（已 clone）
    vector<AlgoResult>      results;        // 该帧推理结果
    float                   infer_fps;
    float                   disp_fps;
    int64_t                 logic_frame_id;
    int64_t                 frame_seq;      // frame 与 results 共同对应的帧序号（二者“同帧”的证明）
    int64_t                 result_age_ms;  // 距上次推理的毫秒数，-1=还没有结果
    bool                    has_results;
    shared_ptr<void>        logic_state;    // 与 frame/results 同一把锁读取，严格配对
    ChannelOnlineState      online_state;   // 快照时刻的在线状态（CH_ONLINE/...）
};
```

---

## API 参考

### 生命周期

```cpp
// 初始化：加载配置、分配状态槽、启动热监控线程
int app_ctrl_init(const std::string &cfgPath);
// 返回 0=成功，-1=配置加载失败

// 反初始化：停止热监控线程、释放控制块
// 调用前必须确保所有采集器和分析器已停止
void app_ctrl_deinit();
```

### 通道数据查询（二次开发常用）

```cpp
// 获取最近一次推理结果（不考虑新鲜度）
vector<AlgoResult> app_ctrl_get_results(int chnId);

// 获取结果，若距上次推理超过 max_age_ms 则返回空（推荐）
vector<AlgoResult> app_ctrl_get_results_fresh(int chnId, int max_age_ms = 200);

// 获取通道显示帧率 / 推理帧率
float app_ctrl_get_disp_fps(int chnId);
float app_ctrl_get_infer_fps(int chnId);  // 直接读 atomic，无锁

// 检查通道是否检测到某类目标（max_age_ms 内有效）
bool app_ctrl_has_target(int chnId, const string &label, int max_age_ms = 200);
int  app_ctrl_get_target_count(int chnId, const string &label, int max_age_ms = 200);

// 获取通道最新的 BGR 帧（已 clone，已加锁）
cv::Mat app_ctrl_get_channel_frame(int chnId);

// 原子获取 (frame + results + logic_state) 配对快照，避免分步读取的竞态
bool app_ctrl_get_channel_snapshot(int chnId, ChannelSnapshot &out);

// 获取通道逻辑名称
string app_ctrl_get_logic_name(int chnId);

// 获取通道跨帧持久化状态（需 static_pointer_cast 转换为实际类型）
shared_ptr<void> app_ctrl_get_logic_state(int chnId);

// 获取时间戳（毫秒，steady_clock 纪元）
uint64_t app_ctrl_get_last_infer_ts_ms(int chnId);
uint64_t app_ctrl_get_last_logic_ts_ms(int chnId);
```

### 全局属性（inline，无锁，零开销）

```cpp
int  app_ctrl_get_chn_nums();       // 配置的通道总数
bool app_ctrl_get_enable_disp();    // 是否启用显示
int  app_ctrl_get_disp_width();     // 显示宽度（已对齐 4 的倍数）
int  app_ctrl_get_disp_height();    // 显示高度（已对齐 2 的倍数）
int  app_ctrl_get_tile_cols();      // 显示网格列数
int  app_ctrl_get_tile_rows();      // 显示网格行数（自动计算）
int  app_ctrl_get_max_fps();        // 全局推理帧率上限
int  app_ctrl_get_total_channels(); // 同 app_ctrl_get_chn_nums()
```

---

## 配置热监控线程

`config_monitor_thread` 在 `app_ctrl_init` 中启动，全程独立运行：

```
循环（每 2 秒或收到 cv_config 通知立即唤醒）
  │
  ├─ 检测 config.json 的 mtime 是否变化
  │    └─ 变化 → 再等一轮（等文件写入稳定后再重载）
  │
  ├─ load_config() 加载新配置到临时对象
  │
  ├─ [锁外] 检查哪些通道的模型路径/类型发生变化
  │    └─ algorithm_reload_channel_model()（耗时，不持锁）
  │
  ├─ [写锁内] g_cfg_reg.sync_fields() 字段级原地同步（不整体赋值，保护指针有效性）
  │    └─ 通道 logic 名变化时：清空 logic_state / draw_cmds / frame_id
  │
  ├─ [锁外] algorithm_update_thresh() 更新检测阈值
  ├─ [锁外] algorithm_update_detect_classes() 更新类别白名单
  ├─ [锁外] analyzer_update_tracker() 更新跟踪器参数
  └─ [锁外] analyzer_reset_tracker_ids() 模型变化时重置轨迹 ID
```

---

## 调试打印宏

`system.h`（位于 `src/` 根目录，非 `core/`）提供受 `debug_display` 配置开关控制的线程安全打印宏：

```cpp
// 在 config.json 的 global 中设置 "debug_display": 1 即可开启
DBG_PRINT("ch%d val=%d\n", chnId, val);
// debug_display=0 时此宏完全零开销（编译器优化消除）
```

---

## 二次开发指南

### 在 logic 函数中查询其他通道

```cpp
// 判断通道 2 最近 500ms 内是否有人
if (app_ctrl_has_target(2, "person", 500)) { ... }

// 获取通道 1 的帧+检测框配对快照（避免分步读取的竞态）
ChannelSnapshot snap;
if (app_ctrl_get_channel_snapshot(1, snap) && snap.has_results) {
    cv::Mat frame = snap.frame;           // 已 clone，可直接操作
    for (auto &r : snap.results) { ... }
}

// 读取通道 0 的自定义 logic_state
auto state = app_ctrl_get_logic_state(0);
if (state) {
    auto my_state = std::static_pointer_cast<MyState>(state);
}
```

### 常见错误与正确做法

| 错误写法 | 正确写法 |
|----------|----------|
| `g_pCtrl->channels_state[i].last_results` 直接读 | `app_ctrl_get_results(i)` |
| `g_pCtrl->config = new_cfg` 整体赋值 | `g_cfg_reg.sync_fields(&old, &new, true)` |
| 在 display_worker 以外修改 `tile_staging` | 不要修改 (A) 组字段 |
| 对多个通道共用一把锁 | 用 `g_pCtrl->chn_mtx[chnId]`，每通道独立 |
| 持写锁期间调用耗时操作（如 `algorithm_reload_channel_model`） | 锁外执行耗时操作，锁内仅做数据同步 |
