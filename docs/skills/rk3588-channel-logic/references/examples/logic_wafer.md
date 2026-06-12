# logic_wafer — 晶圆盘擦拭工序管理（轨迹/覆盖率/动作识别/计时/合规报警）

- **上报**：无（纯本地可视化 + 工序结算）
- **可调参数**：`line_width`(int 像素)、`t_start`(float 秒, 工序开始确认)、`t_end`(float 秒, 工序结束确认)、`coverage_threshold`(float %)、`required_actions`(string, 逗号分隔必做动作)
- **计算区域**：**wafer(class 1) 检测框本身**（动态），不用手画 ROI
- **用到的能力**：检测框当动态区域、进入/结束 dwell 状态机、`cv::polylines` 重建扫过掩码 + 子矩阵 `mask(rect)` 求覆盖率、单色半透明轨迹(`draw_polyline` alpha)、**结算时离线动作识别 `wafer_detect_missing(点集, required)`**(滑窗 + Kåsa 圆拟合 + 逐步行程)、`getTextSize` 右对齐报警、合规结算

## 状态机 / 做什么
模型类别：`0=cleanwiper`(工具尖端)、`1=wafer`(晶圆盘)。区域=wafer 框。

1. **工序开始(T_start)**：cleanwiper 中心进入 wafer 框并**持续 ≥ `t_start` 秒** → 开始（计时/记轨迹/算覆盖）。确认期滤除手短暂进入、反光/闪光误检，期间不画不计（≤400ms 抖动宽限）。
2. **进行中**：每帧把工具中心连成轨迹，按 `line_width` 拓宽成"扫过面积带"，**单色半透明**铺底（不再按动作实时染色——实时分类又难又抖，已去掉）。左上实时显示 覆盖率（**以实时 wafer 框为基准**）/用时/状态。
3. **动作识别只在结算时做**：进行中只记录轨迹点(坐标+编号)、不分类；工序结束时把整条轨迹交给独立函数 `wafer_detect_missing(点集, required)` 离线分析有无漏动作。详见下方算法节。
4. **工序结束(T_end)**：工具离开区域或消失**持续 ≥ `t_end` 秒** → 停止、结算。
5. **结算 + 报警**：
   - 覆盖率（**以"结束那一刻 wafer 框"为基准**）≥ `coverage_threshold` → 覆盖达标；
   - `required_actions` 列表里的动作都出现过 → 动作完整（列表为空则跳过）；
   - 任一不满足 → **右上角红字 `NG` + 原因驻留显示**（直到下一工序开始）；合格显示绿字 `PASS`。

> 抗框抖动：实时显示用当前框、结算用结束那一刻的框（见需求）。屏幕文字用 Hershey 字体不支持中文，故动作名/报警用 ASCII（H/V/ARC、`Coverage 65% < 80%`、`Missing: ARC`）。

## 动作识别（只在结算时离线分析整条轨迹，不实时染色）
工序进行中**只记录轨迹点(坐标+编号)、单色画**，不做实时分类（实时逐帧分类又难又抖，已弃用）。工序结束时，把整条轨迹交给一个独立函数判漏：

**`wafer_detect_missing(pts, required) → 缺失动作 id 列表`**
- 输入1 `pts`：有序轨迹点 `struct WaferTrajPoint{ cv::Point pt; int id; }` 的集合；
- 输入2 `required`：需校验的动作 id 列表(1=横/2=纵/3=弧，0/1/多个；空=不校验)；
- 做法：对整条轨迹**滑动窗口**(WIN=28, STEP=4)逐段判一个标签，统计各动作命中的窗口数，**达到支持度阈值**(≥2 窗口 或 ≥12% 窗口)即认定"出现过"，返回 required 里没出现的。

单窗口判别 `wafer_window_label(p, m)`（几何/运动学，无需训练）：
- **圆弧**：Kåsa 圆拟合**归一化残差** `rms|dist−R|/R<0.12` + **角程** `≥1.0rad` + 非共线(`linearity<0.93`) + 半径合理。残差把真圆弧与"略弯直线/巨圆=直线"分开。
- **横/纵**：**逐步行程** `Σ|dx|` vs `Σ|dy|`（看来回往复方向，抗"擦时整体漂移"——⚠ 勿用 PCA 位置散布判轴，会被横向漂移带偏，曾把纵擦误判成横擦）。
- 门控：`linearity` / `cv::dft` 主频占比 / 反转次数 确认"是擦非漂移"，否则该窗口记未知。

> **离线统计天然更稳**：个别窗口判错不影响"是否出现过"的多数结论；比实时逐帧分类+染色简单可靠得多。每工序只在结算跑一次，开销无所谓。

```cpp
struct WaferTrajPoint { cv::Point pt; int id; };
// 结算时： required = wafer_parse_required(cfg);   // "横擦,纵擦,圆弧擦"/"h,v,arc" → {1,2,3}
//          miss     = wafer_detect_missing(pts, required);   // 返回缺失 id 列表
static std::vector<int> wafer_detect_missing(const std::vector<WaferTrajPoint>& pts,
                                             const std::vector<int>& required) {
    if (required.empty()) return {};
    std::vector<cv::Point> xy; for (auto& q : pts) xy.push_back(q.pt);
    const int n=(int)xy.size(), WIN=28, STEP=4; int cnt[4]={0,0,0,0}, nwin=0;
    for (int e=WIN; e<=n; e+=STEP){ int lab=wafer_window_label(xy.data()+(e-WIN),WIN); if(lab>=1&&lab<=3)++cnt[lab]; ++nwin; }
    if (nwin==0 && n>=6){ int lab=wafer_window_label(xy.data(),n); if(lab>=1&&lab<=3)++cnt[lab]; nwin=1; }
    int need=std::max(2,(int)std::lround(0.12*nwin));
    std::vector<int> miss; for (int a:required) if (cnt[a]<need) miss.push_back(a); return miss;
}
```
`wafer_window_label` / `wafer_motion_features`(PCA `linearity` + 逐步行程 + Kåsa 圆拟合 + Hann+`cv::dft` 主频占比) 全实现见 `src/logic/logic_wafer.cpp`。
> 阈值（窗口28、支持度 2 或 12%、残差0.12、角程1.0、行程比1.15）按现场视频微调。

## 跨帧状态（要点）
```cpp
struct WaferState {
    std::vector<cv::Point> trajectory;   // 轨迹点(模型坐标, 顺序即编号; 单色绘制, 不再存逐点动作)
    cv::Mat swept_mask; cv::Size mask_size;           // 扫过掩码(每帧由轨迹重建)
    cv::Rect region_box; bool have_region=false;      // wafer 框=区域(缓存最近一次)
    uint64_t enter_ms=0, region_last_ms=0;            // 开始确认 / 在区域内时刻(开始&结束共用)
    bool op_active=false; uint64_t op_start_ms=0;     // 工序进行中 / 开始时刻
    bool have_result=false, last_pass=true; float last_coverage=0, last_op_sec=0; std::string last_msg; // 结算
};
```
完整实现见 `rk3588_yolo/src/logic/logic_wafer.cpp` 的 `logic_wafer`（同文件内含 helper：`wafer_action_name`、`wafer_motion_features`、`wafer_window_label`、`wafer_parse_required`、`wafer_detect_missing`）。关键流程：找 wafer 框→guard→扫描 cleanwiper(更新 region_last_ms)→开始确认→记点(进行中**不**实时分类)→重建掩码算实时覆盖→结束判定+结算(覆盖率 + 离线判漏动作)→单色半透明轨迹绘制 + 左上文字 + 右上报警。

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_wafer.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_wafer", logic_wafer);`。
- 5 个参数四处对齐（均热重载）：`config.h` 字段 + `config_init.cpp` `REG_C` + logics.json 声明 + 逻辑里 `ctx->config->xxx` 现读。
  - `line_width`(INT) `t_start`(FLOAT) `t_end`(FLOAT) `coverage_threshold`(FLOAT) `required_actions`(STRING)
- **不需要画 ROI**；必须保证模型稳定检出 wafer(class 1)，否则一直 `waiting for wafer (region)`。
- 不上报：logics.json 不写 `report`。

## 复用提示
- **离线判漏动作(滑窗投票)**：把"实时逐帧分类"换成"结算时整段滑窗统计"——独立函数 `wafer_detect_missing(点集, required)`，对全轨迹滑窗逐段判 + 支持度阈值定"出现过"。比实时简单且稳(个别窗口判错被多数掩盖)。通用于任何"一次操作里有没有做过某类动作"。
- **单窗口运动分类(几何/运动学)**：Kåsa 圆拟合(残差/角程)定圆弧、**逐步行程 Σ|dx|/Σ|dy| 定横/纵**(抗整体漂移；勿用 PCA 位置散布判轴，会被横向漂移带偏)、`linearity`/`cv::dft`/反转作"是擦非漂移"门控。无需训练。
- **dwell 双门槛 + 结算**：`t_start` 进入确认防误触发、`t_end` 结束确认；结束时一次性算覆盖率/查漏动作→驻留报警。"一次操作的开始/结束/合规"通用范式。
- **右对齐文字**：`getTextSize` 量宽 → `x = 帧宽 - 宽 - 边距`，因位置与字号同比缩放，缩放后仍右对齐。

## 已知取舍
- 区域=当前帧 wafer 最高分框（缓存）。手遮挡致框抖动/缩小时覆盖率分母会波动；需要更稳可在工序开始锁定框 / 滑动平均 / 取历史最大框。
- 轨迹**单色**绘制(不再按动作染色)——实时分类难且抖，已弃用；动作只在**结算时离线**判漏。
- 屏幕文字 ASCII（Hershey 无中文）；`required_actions` 输入支持中文 横擦/纵擦/圆弧擦 或 h/v/arc，显示/报警用 H/V/ARC。
- 离线动作识别为几何/统计法，无需训练；极快或混合动作仍可能误判，阈值(窗口28、支持度2或12%、残差0.12、角程1.0、行程比1.15)按现场视频微调。
- 确认抖动宽限 `region_grace_ms=400ms`、动作窗口 `28`、最小位移 `3px` 为硬编码常量，需要可调可参数化。
