# logic_default — 空白逻辑示例

- 源码：`vision_analysis/src/logic/modules/logic_default/logic.cpp`
- 上报：无
- 参数、动作、状态：无

`logic_default` 是一个可选、可删除的普通示例模块。它不修改检测结果、不增加绘制、不产生告警；模型和播放器的通用流程仍正常运行。配置不写 `channels[].logic` 时，框架原生进入“无后处理”状态，效果与选择本示例相同，因此系统并不依赖该模块兜底。

```cpp
#include "logic/core/logic_common.h"

static void logic_default(ChannelContext *)
{
}

REGISTER_LOGIC(logic_default);
```

`logics.json` 对应条目：

```json
{
  "label": "空白逻辑示例",
  "report_fields": [],
  "params": []
}
```

新 logic 可以从这个文件复制结构；学习统一上报、媒体分层和 Web 接线先看 `logic_upload_teach.md`，实现真实 ROI 进入告警再看 `roi-alarm-pattern.md`。
