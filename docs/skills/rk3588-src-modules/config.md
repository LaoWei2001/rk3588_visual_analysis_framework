# `src/config`：配置加载、校验与字段注册

## 文件职责

- `config.h`：`AppConfig`、`ChannelConfig`、流、模型、ROI 和全局 logic 的结构定义及 C++ 默认值。
- `config.cpp`：解析 JSON、解析统一的 `models[]`、`roi_zones[]` 和 `report_policy`，继承运行默认并排序启用通道。
- `config_init.cpp`：用 `REG_G`/`REG_C` 注册可做通用同步的字段。
- `config_registry.*`：按类型从 cJSON 读取字段，并在热重载时同步已注册字段。
- `config_validator.*`：首次加载完整校验；热重载使用 critical 校验。

## JSON 结构

```json
{
  "global": {
    "enable_display": true,
    "max_fps": 30,
    "global_logics": [
      {"enable": true, "logic": "global_default", "channels": [], "poll_interval_ms": 100}
    ]
  },
  "channels": [
    {
      "id": 0,
      "enable": true,
      "infer_enable": true,
      "stream": {"src_type": "usb", "device": "/dev/video81"},
      "models": [
        {"id": "det-1", "enable": true, "model_type": "yolov8_det",
         "model_path": "./assets/model.rknn", "label_path": "./assets/labels.txt",
         "obj_thresh": 0.4, "nms_thresh": 0.45, "detect_classes": [], "npu_core": -1}
      ],
      "roi_zones": [{"name": "entrance", "polygon": [[0.1,0.1],[0.9,0.1],[0.9,0.9]]}]
    }
  ]
}
```

`global_logics` 位于 `global` 对象内，不在顶层。`stream.src_type` 必填，只支持 `rtsp`、`file`、`usb`；USB 优先取 `stream.device`，其他源取 `stream.url`。启用通道必须有有效 location。`channels[].logic` 是可选后处理模块名；省略或设为空字符串时不执行任何业务模块，视频仍显示，模型结果仍由框架绘制。

## 继承与多模型

模型类型、路径、标签、版本、阈值、类别过滤和 NPU 核心只存在于 `channels[].models[]`。一个条目就是单模型，多个有效条目会在同一帧运行并合并结果；空数组表示无模型通道。模型阈值缺省时继承 `global.obj_thresh/nms_thresh/detect_classes`。`infer_enable=false` 是整个通道的推理总开关；`models[].enable` 控制单个模型。旧的通道顶层模型字段会被明确拒绝。

ROI 唯一格式是 `channels[].roi_zones[]`，顶点为 0–1 归一化坐标。加载后由 analyzer 按模型输入尺寸生成 `RoiZone`。

`report_policy` 和 `report_parameters` 以通用 JSON 保存到字符串字段，允许 Web 增加上报参数而不扩充 C++ 结构。录像开关、前后时长、帧率和叠加方式只保存在 `report_policy`，加载时派生为非持久化运行参数；默认值分别为 3 秒、3 秒、15 FPS 和 `custom`。

## 新增公共配置字段

以下流程只适用于所有通道/系统共同拥有的新底层配置。某个 `logic_xxx` 独有的普通业务参数应使用模块 `logic.json.parameters + ctx->param_*()`，见 `../rk3588-channel-logic/references/adding-config-parameter.md`，不要扩展中央结构。

1. 在 `AppConfig` 或 `ChannelConfig` 添加字段和默认值。
2. 在 `config_init.cpp` 使用匹配类型的 `REG_G` 或 `REG_C` 注册。
3. 在 `config_validator.cpp` 加范围、枚举或组合约束。
4. 若字段是嵌套对象/数组（如 `stream`、`models`、ROI、global logic），在 `config.cpp` 显式解析，并在 `app_ctrl.cpp` 明确热重载行为；注册表不能自动处理任意嵌套结构。
5. 若 Web 会编辑该公共字段，同步更新对应 Web 节点和配置序列化；不要把系统字段伪装成某个 logic 的模块参数。

## 热重载边界

配置监控在文件 mtime 改变且内容稳定后加载临时配置。它拒绝通道数量、排序或 id 变化；这类拓扑变化必须重启。当前可热更新普通注册字段、阈值、类别、tracker、logic（并清空旧状态/结果/绘制）、模型、ROI、global logic 实例和 stream（重建采集器并保持共享源关系）。

显示缓冲尺寸、tile 布局、线程数量等虽然可能被解析/同步，但其相关资源是在启动时分配的；修改这类结构性参数应重启，不要仅依赖热重载。

读取全局配置时使用 `g_pCtrl->mtx` 的 pthread 读锁；不要把它误写成 `std::shared_mutex`。通道运行态另由 `chn_mtx[]` 保护。
