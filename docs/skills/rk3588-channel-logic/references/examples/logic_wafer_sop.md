# logic_wafer_sop — 晶圆湿法清洗 SOP 合规（花篮进槽顺序 / 朝向 / 抖动）

- **上报**：无（纯画面"逐工序点亮"提示，不结算不上报）
- **可调参数**：`sop_sequence`(string)、`basket_normal_label`(string)、`basket_abnormal_label`(string)、`sop_shake_amplitude`(int)、`sop_shake_min_count`(int)、`sop_enter_sec`(float)、`sop_dir_sec`(float)
- **计算区域**：本通道按 `sop_sequence` 各名字**手画的命名 ROI 区域**（多 ROI，靠 `ctx->roi_by_name(名字)` 取）
- **用到的能力**：命名多 ROI（`roi_by_name`）、进入区域防抖、朝向（类别）切换防抖、抖动（中心 Y 往复）计数、freetype 中文绘制、跨帧状态、长时间离场自动复位

## 做什么
管理"装晶圆的特氟龙花篮依次转移过几个清洗槽"。全程**逐工序点亮**完成度（不做结算）：左上一列清单，每行一开始红色、达成即变绿。默认 6 行（4 槽 + 抖动 + 朝向）：

1. **各槽（`sop_sequence`）**：花篮中心在某槽内**持续 `sop_enter_sec` 秒**才算"真正进入"（滤除转移时短暂划过），该行变绿；漏掉/未到的槽保持红。进入**首槽**=开新一轮，清空上一篮清单。
2. **抖动次数**：进入**最后一个槽（纯水槽）**后才计数，跟踪框中心 Y 上下往复，每达 `sop_shake_amplitude` 记一次；次数 ≥ `sop_shake_min_count` → 变绿。
3. **朝向**：默认绿"方向未变化"；花篮类别（`basket_normal_label` / `basket_abnormal_label`）持续 `sop_dir_sec` 秒翻转后 → 红"方向变化"。

另：高亮花篮当前实际所在槽（粗多边形框）；花篮框/中心按已确认朝向上色（正常绿 / 翻转红）；花篮长时间离场（`RESET_MS=6000`）自动复位本轮。

> 槽名/抖动次数/方向等中文直接经 freetype 渲染显示（`draw_text` 支持中文）。坐标系=模型输入尺寸(640)。**纯画面提示，不上报。**

## 实现要点（完整代码见 `src/logic/logic_wafer_sop.cpp`）
- 文件私有 helper `sop_split_csv(s)`：逗号分隔字符串 → 去空白片段（解析 `sop_sequence`）。
- 跨帧状态 `WaferSopState`（文件私有）：`visited`（已点亮槽集合）、进入防抖 `cand_zone`/`cand_since_ms`、朝向防抖 `dir_cand`/`dir_confirmed`/`reversed_seen`、抖动 `in_shake`/`shake_count`/`ext_y`/`y_dir`、`last_seen_ms`。
- 主流程：
```cpp
static void logic_wafer_sop(ChannelContext *ctx)
{
    // 1) 参数每帧现读（热重载）：sop_sequence 拆成 seq[]，normLab/abnLab，shakeAmp/shakeMin，
    //    enter_ms = sop_enter_sec*1000，dir_ms = sop_dir_sec*1000；nseq<2 时提示并返回
    // 2) 找"花篮" = 置信度最高的 normLab/abnLab 检测；basket_reversed = (类别==abnLab)
    // 3) 花篮中心落在哪个 seq 命名槽：遍历 seq[i] → ctx->roi_by_name(seq[i]) → pointPolygonTest
    // 4) 花篮长时间离场 → WaferSopState() 复位
    // ② 朝向防抖：新类别持续 dir_ms 才确认翻转/恢复（reversed_seen=true 表示本轮出现过翻转）
    // ① 进入防抖 + 逐槽点亮：同一槽持续 enter_ms 才"确认进入"→ visited.push_back(z)；
    //    进入首槽(z==0)=开新一轮先清空；进入最后一个槽=开始抖动计数
    // ③ 抖动检测：纯水槽内跟踪中心 Y，往复幅度达 shakeAmp 记一次 shake_count
    // 画面：逐槽多边形(已点亮绿/未到黄，当前槽粗框)+槽名；花篮框按朝向上色；
    //       左上清单逐行 红/绿（各槽 + "抖动次数 N/min" + "方向未变化/变化"）
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_wafer_sop.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_wafer_sop", logic_wafer_sop);`。
- logics.json：`{ "name": "logic_wafer_sop", "label": "晶圆清洗SOP(花篮顺序/朝向/抖动)", "params": [ sop_sequence / basket_normal_label / basket_abnormal_label / sop_shake_amplitude / sop_shake_min_count / sop_enter_sec / sop_dir_sec ] }`（7 个参数详见 logics.json）。
- 7 个参数四处对齐（均热重载）：`config.h` 的 `ChannelConfig` 字段 + `config_init.cpp` 的 `REG_C` + logics.json 声明 + 逻辑里 `ctx->config->xxx` 现读。
- **必须画命名 ROI**：网页该通道「ROI 检测区域」节点里，按 `sop_sequence` 的每个槽名各画一个区域并**命名一致**（如 `去胶槽1`/`去胶槽2`/`IPA槽`/`纯水槽`）。名字对不上则该槽永不点亮。
- 模型类别须含花篮正常 / 翻转两类（默认 `bkt_normal` / `bkt_abnormal`，须与 labels.txt 一致）。
- 不上报：logics.json 不写 `report`。

## 复用提示
- **命名多 ROI + 顺序点亮**：用 `ctx->roi_by_name(名字)` 把"业务步骤"绑到各自命名区域，`visited` 集合记完成度——任何"按顺序经过若干工位/区域"的 SOP 都套这套。
- **进入/朝向双防抖**：候选状态须持续 N 毫秒才"确认"，滤掉瞬时误检/短暂划过——比单帧判定稳得多，可迁移到任何"状态需稳定一段时间才算数"的场景。
- **往复计数**：抖动用"中心坐标在某轴上的极值翻转 + 幅度阈值"计数，可迁移到摇晃/点动/挥手等周期动作。
- 它是"**全程实时点亮、不做结算/不报警**"范式；要改成"结束后判合格 + 上报"，参考 `logic_wafer` 的 dwell 结算 + `alarm_uploader_enqueue`。
