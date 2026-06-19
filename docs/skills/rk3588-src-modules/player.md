# 显示模块 (player/)

> 基于 GTK3 的多路视频拼接显示层：将各通道的 NV12 帧经 RGA 硬件缩放后拼接成网格，叠加检测框、ROI、FPS 等信息，输出到 X11 窗口。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `display.h` | `Display_t` 显示描述符、`dispBufferMap`、`display`、`render_overlays` 等接口声明 |
| `display.cpp` | GTK3 窗口创建、双缓冲 framebuffer、tile 拼接、叠加渲染实现 |
| `text_overlay.h / text_overlay.cpp` | freetype 中英文文字渲染（`draw_text_unicode`）：画面/上报图统一中文绘制 |
| `rtsp_streamer.h / rtsp_streamer.cpp` | 内置 RTSP 推流服务（无显示器时经 VLC / 配置平台查看拼接画面）|

---

## 架构设计

### 双缓冲机制

为避免 GTK 读取 framebuffer 时与写入线程产生撕裂，显示模块采用双缓冲：

```
display_worker 线程（每通道一个）
    │
    ├─ RGA 缩放：通道原始帧 → tile 尺寸（如 960×540）
    ├─ render_overlays()：在 tile 上绘制检测框/ROI/FPS
    ├─ 写入 back_buffer 对应 tile 区域
    └─ display_swap_buffer()：交换前后缓冲区

GTK draw 回调（UI 线程）
    └─ 从 front_buffer 读取并绘制到 GdkWindow
```

`display_lock() / display_unlock()` 确保 swap 操作与 GTK 绘制不重叠。

### 网格布局

屏幕分为 `tile_cols × tile_rows` 的网格，每个格子分配给一个通道：

```
┌─────────┬─────────┐
│ 通道 0  │ 通道 1  │   tile_cols=2, tile_rows=2
├─────────┼─────────┤
│ 通道 2  │ 通道 3  │
└─────────┴─────────┘
```

每个 tile 的尺寸 = `(disp_width / tile_cols) × (disp_height / tile_rows)`

---

## API 参考

### 初始化

```cpp
// 分配 GTK 显示缓冲区（在 analyzer_init 之前调用）
// dispDesc: 窗口标题、位置、总分辨率
char **dispBufferMap(Display_t *dispDesc);
// 返回双缓冲区的指针数组，失败返回 nullptr

// 启动 GTK 主循环（阻塞，窗口关闭时返回）
int display(Display_t *dispDesc);
```

### 叠加渲染

```cpp
// 在 tile 区域上绘制检测框、ROI、FPS 等叠加信息
// RenderParams 定义在 channel_logic.h，调用方须先 include channel_logic.h
void render_overlays(cv::Mat &screen_roi, const RenderParams &p);
```

`RenderParams` 字段：

```cpp
struct RenderParams {
    int     chnId;            // 通道号
    int     srcWidth, srcHeight;  // 原始视频流分辨率（用于坐标缩放）
    int     inputW,   inputH;     // 模型输入尺寸（如 640×640）
    float   disp_fps, infer_fps;  // 帧率
    int64_t result_age_ms;        // 距上次推理的毫秒数（用于速度外推）
    int     show_fps;             // 是否在画面上显示 FPS（0/1）
    uint8_t target_mask;          // 渲染目标：DISPLAY=0x01，UPLOAD=0x02，ALL=0x03

    const vector<RoiZone>     *roi_zones;  // 本通道全部 ROI 区域（各含名字+模型坐标多边形），逐个绘制
    const vector<AlgoResult>  *results;    // 检测结果
    const vector<DrawCommand> *draw_cmds;  // logic 自定义绘制指令
};
```

坐标缩放规则：
- 检测框/ROI 坐标原本在模型输入坐标系（如 640×640）
- 渲染时先从模型坐标系缩放到原始流坐标系，再从原始流坐标系缩放到 tile 尺寸
- 通过 `ctx->render_params()` 可快速构建 `RenderParams`（在 logic 函数中）

### 同步原语

```cpp
void display_lock();    // 获取显示锁（阻塞）
void display_unlock();  // 释放显示锁

char *display_get_back_buffer();  // 获取后缓冲区指针
void display_swap_buffer();       // 原子交换前后缓冲区
```

---

## 配置参数

| 参数 | 说明 |
|------|------|
| `enable_display` | `true`=开启 GTK 窗口，`false`=纯后台无显示运行 |
| `disp_width` | 显示分辨率宽（建议与显示器分辨率一致） |
| `disp_height` | 显示分辨率高 |
| `tile_cols` | 每行显示的通道数 |
| `tile_rows` | 行数（为 0 时自动计算） |
| `performance_display` | 性能统计总开关：画面 FPS 叠加 + 终端性能日志（`display.cpp` 中与 `p.show_fps` 共同决定 FPS 是否上屏） |

---

## 二次开发指南

### 无显示模式运行

将 `enable_display` 设为 `false` 可完全跳过显示模块，适用于服务器部署场景：

```json
{
  "global": {
    "enable_display": false
  }
}
```

此时主循环进入 `while(isRunning)` 等待模式，推理和上传功能不受影响。

### 自定义叠加内容

业务逻辑通过向 `ctx->draw_cmds` 添加 `DrawCommand` 来定制显示内容，`render_overlays` 会统一渲染这些指令。所有坐标使用模型坐标系（640×640），无需关心屏幕分辨率：

```cpp
// 在 logic 函数中
draw_rect(ctx, cv::Rect(100, 100, 200, 150), cv::Scalar(0, 0, 255), 3);
draw_text(ctx, "ALARM", cv::Point(100, 90), cv::Scalar(0, 0, 255), 0.8, 2);
```

### 修改检测框默认颜色

`AlgoResult::box_color` 为 `(-1,-1,-1)` 时使用默认颜色（按类别 ID 自动分配色板）。在 logic 函数中可覆盖：

```cpp
for (auto &r : *ctx->results) {           // ctx->results 是指针，先解引用
    if (r.label == "car")
        r.box_color = cv::Scalar(0, 128, 255);  // 橙色
}
```

### 远程 SSH 运行注意事项

SSH 连接时 `DISPLAY` 环境变量可能为空，程序会自动设置 `DISPLAY=:0`（见 `main.cpp`）。如果目标机器没有 X11 显示服务，请设置 `enable_display: false`。
