# 安全穿戴（PPE）视觉检测算法逻辑说明

> 适用对象：`logic_ppe_guard` 通道逻辑 + 6 类 YOLOv5 检测模型（`assets/best.rknn`）
> 代码位置：`/userdata/rk3588_visual_analysis_framework/projects/modules/logic_ppe_guard/`
> 文档版本：2026-11-16（对应 `glove_min_score` 上线版本）

---

## 一、这套逻辑要解决什么问题

现场要求：**一个人只要"没戴安全帽 / 没穿工作服 / 没戴手套"任意一项成立，就报警。**

难点不在"检出装备"，而在于：

1. 模型只给检测框，不给"这个人穿了什么"的结构化结论——必须自己把框归属到人、归属到部位。
2. 6 类模型里**没有** `no_workwear` 这类负类，工装只能靠"缺正类"推断，推断天然容易误报。
3. 现场遮挡多（设备、护栏、别人），"看不见"不等于"没戴"。
4. 检测器会漏检、会误检，单帧结论不可信，必须多帧累计。

整套算法就是围绕这四点设计的：**归属 → 门控 → 多帧确认 → 可撤销**。

---

## 二、代码分层

| 文件 | 职责 | 是否依赖框架 |
| --- | --- | --- |
| `ppe_guard.h` / `ppe_guard.cpp` | 纯逻辑内核：部位状态机、证据窗口、归属与几何判定、去重 | 只依赖 `types.h`，可被单测直接实例化 |
| `logic.cpp` | 通道适配层：读参数、ROI 过滤、画框、上报事件、发布输出 | 依赖 `ChannelContext` |
| `logic.json` | 41 个参数定义 + 7 个输出 + 4 个动作 + 事件类型 | 被 Web 控制台读取 |
| `engine/tests/ppe/test_ppe_guard.cpp` | 167 条断言的回归测试 | 仅依赖内核 |

分层的意义：所有"会不会误报"的判断都在内核里，改判据不用碰框架，跑 `./build/engine/ppe_tests` 就能验证。

---

## 三、输入：6 类模型的标签语义

模型输出 33 通道（`3 anchors × (5 + 6)`），标签文件 `assets/labels.txt`，顺序固定：

| 索引 | 标签 | 含义 | 逻辑层角色 |
| --- | --- | --- | --- |
| 0 | `person` | 人体 | 判定的主体（track 载体） |
| 1 | `helmet` | 已戴安全帽 | 安全帽**正类** |
| 2 | `hand_gloved` | 已戴手套 | 手套**正类** |
| 3 | `hand_bare` | 裸手 | 手套**负类（直接证据）** |
| 4 | `workwear_on` | 已穿工装 | 工装**正类** |
| 5 | `head` | 头部（露头） | 安全帽**负类（直接证据）** |

>> **重要**：`head` 是"未戴帽"的直接证据，不是中性类。训练时 `head` 必须只标"没戴帽的裸头"，如果戴帽的人头也被标成 `head`，安全帽这项就会像 08:31 那样大面积误报。

现场模型配置（`/opt/ai_apps/test1025/assets/depth_multi_model.json`）：

```json
{"id": "helmet", "enable": true, "model_type": "yolov5",
 "model_path": "assets/best.rknn", "label_path": "assets/labels.txt",
 "obj_thresh": 0.3, "nms_thresh": 0.45, "npu_core": 0}
```

- `obj_thresh = 0.3`：模型自身先过滤一遍，低于 0.3 的框不会进入逻辑层。
- **没有挂 pose 模型**，所以姿态关键点全部不可用——这一点直接决定了下面几个参数的取值（见第十节）。

---

## 四、一帧的完整处理流程

每帧调用一次 `Guard::update(results, now_ms, config)`，按顺序做 11 件事：

```
① 时间回退检查
② 选人（track_id / 分数 / 标签 / 同帧去重）
③ 会话重置判定（断开太久 → 清空历史）
④ 绑定姿态、计算部位可核验性
⑤ 登记正类框（戴好了 / 穿好了）
⑥ 计算头部 / 躯干是否被遮挡（缺正类推断的前置条件）
⑦ 采集违规框（部位标签匹配 + 分数门槛）
⑧ 同类框去重（IoU）
⑨ 逐框归属到人 → 归属到部位 → 几何门控 → 写"本帧证据"
⑩ 缺正类推断补证据（工装；安全帽可选）
⑪ 每部位证据窗口 → 确认状态机 → 生成 Evaluation
```

### ① 时间回退

```cpp
if (clock_initialised_ && now_ms < last_ms_)
    people_.clear();   // 时间回退：旧证据的时间基准全部失效
```

视频回放、循环播放、系统时间跳变时会出现 `now_ms` 变小，此时清空所有历史，避免"未来证据"卡住状态。

### ② 选人

一帧里可能有多个检测结果，只有同时满足下面条件的才是"人"：

```cpp
if (result.track_id < 0 || result.score < config.person_min_score) continue;  // 0.35
if (!matches_label(result.label, config.person_labels)) continue;             // person
// 同一帧同一 track_id 只取第一个框
```

`track_id` 由框架的跟踪器给出，是"同一个人跨帧"的唯一依据。**没有 track_id 的人不参与判定**（无法多帧累计证据）。

### ③ 会话重置

```cpp
if (person_state.last_seen_ms != 0 && now_ms - last_seen_ms > reacquire_gap_ms)  // 3000ms
    person_state = PersonState{};   // 断开过久：按新的作业会话重新积累
```

同一个人离开画面 3 秒以上再回来，之前的证据和已上报标记全部作废——否则"早上报过一次"会让他整班都不再报。

### ④ 绑定姿态 + 部位可核验性

`find_pose_for()`：优先取 `track_id` 相同的姿态结果，其次取"框中心落在人体框内"的姿态结果。

`fill_visibility()` 按 COCO17 关键点判定各部位能否核验（阈值 `keypoint_min_score = 0.3`）：

| 部位 | 判定条件 |
| --- | --- |
| 头部（安全帽） | 鼻（0）可见，或左右眼（1、2）同时可见 |
| 躯干（工装） | 左右肩（5、6）同时可见 **且** 左或右髋（11、12）可见 |
| 左手 / 右手 | 左腕（9）/ 右腕（10）可见 |

**现场没有 pose 模型 → 所有 `visible[]` 全是 false → 所有部位在画面上显示为 `-`（未知）。** 这是设计允许的降级，此时判定完全依赖检测框本身（负类直采 + 缺正类推断）。

### ⑤ 登记正类框

先于违规框处理，目的是让"穿好了"的证据在**同一帧**内就能否决"没穿"的误判（模型常在同一个物体上同时给出互斥类别）。

```cpp
if (result.score < config.positive_min_score) continue;   // 0.35，比违规门槛低得多
```

正类用独立的低门槛 `positive_min_score = 0.35`，原因：现场 `workwear_on` 实测只有 0.38～0.53、`hand_gloved` 只有 0.59～0.72。**如果用 0.75 收正类，就会把"明明穿了"当成"没穿"——这正是 v8 首次上线时误报的根因。**

三类正类的归属方式不同：

| 正类 | 归属函数 | 特殊处理 |
| --- | --- | --- |
| `helmet` | `credit_helmet_shared()` | **共享信用**：搜索区内有帽子框的人全部记"戴了" |
| `hand_gloved` | `nearest_person_for_hand()` + `resolve_hand_side()` | 分到具体某只手；分不清则放弃 |
| `workwear_on` | `unique_person_for_box()` | 严格归属，归属不明即丢弃 |

`credit_helmet_shared()` 的存在原因：两人并肩站时，一个帽子框只会被归给一个人，另一人就会因"没帽子"被推断成没戴——宁可两人都算戴了（漏报），也不能误报。

### ⑥ 遮挡判定（缺正类推断的前置条件）

只有"该看的区域确实看得见"，"没看到"才能当作证据。

**头部可核验**（`head_checkable`，仅当 `helmet_infer_by_absence = true` 时计算）：

```cpp
if (box.height < 60) continue;                 // 人太小，头部只有几像素
if (box.y < 6) continue;                       // 头顶贴画面边缘，可能被裁掉
if (detection->score < absence_min_person_score) continue;   // 0.6，人框本身不可信
// 头部区域被别人的人体框盖住 > 40% 面积 → 不可核验
```

**躯干可核验**（`torso_checkable`，仅当 `workwear_require_positive = true` 时计算）：

```cpp
if (box.height < workwear_min_person_height) continue;        // 80px
if (detection->score < absence_min_person_score) continue;    // 0.6
// 躯干区（人框 25% ~ 75% 高度）被别人的框盖住 > 40% → 不可核验
```

这两组门槛是"缺正类推断"不误报的关键闸门。

### ⑦ 采集违规框

```cpp
// 1) 标签必须匹配某个已启用部位的违规标签
// 2) 分数门槛：只手部标签命中 → glove_min_score(0.5)
//              其他 → violation_min_score(0.75)
```

**手套为什么单独用 0.5：** 现场手部框实测只有 0.59～0.72，卡 0.75 等于"未戴手套"这项永远不触发。代价是手部误检更容易变成报警（见第十一节）。

### ⑧ 同类去重

同一件物品常被输出多个重叠框。按分数降序，**同 label 且 IoU ≥ `dedupe_iou`(0.6)** 的只保留最高分那个，其余计入 `duplicate_observations`。

不同类别之间不互斥（`head` 和 `helmet` 同时出现是正常现象，靠正类否决处理）。

### ⑨ 归属 + 门控

这是整套算法最核心的一步，逐框执行：

```
归属到人 → 跨帧一致性检查 → 归属到部位 → 正类否决 → 可见性检查 → 几何门控 → 写入本帧证据
```

**归属到人**

| 框类型 | 函数 | 规则 |
| --- | --- | --- |
| 手部（`hand_bare`） | `nearest_person_for_hand()` | 先严格 containment；失败则取"到人框外沿距离 ≤ `hand_attach_ratio`(0.6) × 人高"的最近者 |
| 安全帽（`head`） | `unique_person_for_box()` | 人框**向上外扩** `helmet_top_ratio`(0.35) × 人高后再判 containment |
| 工装（`no_workwear`，本现场未用） | `unique_person_for_box()` | 严格 containment |

`nearest_person_for_hand()` 是 v12 补的：人手伸出去时手框中心落在人框外，"检出裸手也不报警"。放宽到"最近的人 + 距离上限"后解决。

`unique_person_for_box()` 的语义：中心同时落在**两个或以上**人框内 → 返回 -1（归属不明，丢弃），宁可漏不可错。

**跨帧一致性**

```cpp
if (person.detection->frame_id != result.frame_id) { ++rejected_observations; continue; }
```

检测框和人员框必须来自同一帧，防止上一帧的框被算到这一帧的人头上。

**手部三态**（`resolve_hand_side`）：

```cpp
// 没有姿态 / 双腕都不可见：
if ((!left_ok && !right_ok) || pose == nullptr)
    return config.glove_require_pose ? HAND_REJECTED : HAND_UNRESOLVED;

// 只有一只腕可见：距离 < glove_wrist_ratio × 人高 → 归该侧，否则丢弃
// 双腕都可见：两侧距离都超限 → 丢弃
//             两距离接近（差 < 0.35 × 腕间距）→ HAND_UNRESOLVED（不猜左右）
//             否则归近侧
```

现场 `glove_require_pose = false` 且没有 pose 模型，所以**所有裸手框都走 `HAND_UNRESOLVED`**，记为"不分左右手"的兜底证据（`hand_unsided`）。

**几何门控**（`geometry_ok`，`geometry_gate = true`）

- 安全帽：
  - 框中心相对人框的纵向位置必须在 `[-0.35, +0.35]`（允许高出头顶，不能长到腰上）
  - 框面积 / 人框面积必须在 `[0.01, 0.12]`
  - 有姿态时：框中心到最近鼻/眼关键点的距离 ≤ `helmet_head_ratio`(0.35) × 人高
- 工装：框中心纵向位置必须在人框的 `[0.25, 0.75]` 区间
- 手套：几何约束在 `resolve_hand_side` 里（腕点距离）

### ⑩ 缺正类推断

只对"可核验但没看到正类"的人补一条证据：

```cpp
if (helmet_infer_by_absence && enabled[HELMET] && head_checkable && !positive_seen[HELMET])
    violation_seen[HELMET] = true;

if (workwear_require_positive && enabled[WORKWEAR] && torso_checkable && !positive_seen[WORKWEAR])
    violation_seen[WORKWEAR] = true;
```

现场配置：**工装开启（`workwear_require_positive = true`），安全帽关闭（`helmet_infer_by_absence = false`）**。

- 工装必须靠推断，因为模型没有 `no_workwear` 负类。
- 安全帽不推断，因为模型有 `head` 负类，直接采信更可靠；缺帽推断在 08:31 实测中因遮挡/漏检产生大量误报，已关闭。

### ⑪ 证据窗口 → 确认状态机

每个"人 × 部位"维护一条时间戳队列：

```cpp
struct PartEvidence {
    std::deque<uint64_t> violations;  // 最近的违规观测时间点
    bool confirmed = false;           // 是否已达确认门槛
};
```

**写证据**（本帧该部位有违规）：

```cpp
++outcome.candidate_observations;
push_evidence(evidence.violations, now_ms, evidence_gap_ms);   // 250ms 内不重复计
```

**清理**：超过 `evidence_window_sec`(4s) 的旧证据被丢弃。

**确认条件**（三个必须同时满足）：

```cpp
required_observations >= 1                        // 3 次
&& times.size() >= required_observations           // 至少 3 条
&& times.size() >= 2                               // 至少两条（不能单帧）
&& times.back() - times.front() >= required_span_ms // 跨度 ≥ 1000ms
```

即：**4 秒窗口内至少 3 次独立观测、时间跨度 ≥ 1 秒**，才确认违规。这个设计挡的是随机噪声；挡不住系统性误判（类别对了位置错了），所以还需要几何门控。

**撤销**（v9 补的关键机制）：

```cpp
if (positive_seen[part] && (evidence.confirmed || !evidence.violations.empty())) {
    evidence.violations.clear();
    evidence.confirmed = false;
    person_state.reported = false;   // 允许重新上报
}
```

看到"戴好了/穿好了"的正类框，立刻撤销该部位已确认的违规。理由：检测器漏检一两帧就让人永久挂红是不可接受的。

---

## 五、部位状态与显示

### 状态定义

| 状态 | 字符 | 含义 |
| --- | --- | --- |
| `PART_UNKNOWN` | `-` | 遮挡、模糊、无姿态、归属不明：**不可核验，不参与报警** |
| `PART_CLEAR` | `O` | 本帧可核验（有姿态关键点 **或** 看到正类框）且无违规证据 |
| `PART_CANDIDATE` | `?` | 有违规证据，但未达确认门槛 |
| `PART_VIOLATION` | `X` | 已达确认门槛 |

> `O` 表示"ok"，不用 `.` 是因为 720p 现场画面上 `.` 和 `-` 几乎分不清。

### 画面元素

- **人员框颜色**：红 `(0,0,255)` = 已确认违规；橙 `(0,165,255)` = 有候选证据；绿 `(0,220,0)` = 正常
- **徽标**：画在框下边缘外侧 `#2 H O T- L- R-`（H 安全帽 / T 工装 / L 左手 / R 右手）
- **左上角汇总**（调试模式）：
  - 第一行 `PPE 候选0 确认0 人员1`（确认数>0 时变红）
  - 第二行 `丢弃0 去重0 否决0`
  - 第三行 `灵敏度0.75`
- **底部标签统计** `标签: helmet1 person1 workwear_on1`（本帧实际出现的类别，用于核对 `label_path` 是否与权重匹配）
- **自检告警** `有检测框但没有人员类别: 检查 person_labels 与模型 label_path`（连续 25 帧）

---

## 六、输出与上报

### 发布变量（每帧）

| 变量 | 类型 | 含义 |
| --- | --- | --- |
| `ppe_alarm` | bool | 是否存在已确认违规人员 |
| `confirmed_people` | int | 已确认违规人数 |
| `candidate_observations` | int | 本帧新增候选证据数 |
| `rejected_observations` | int | 被归属/几何门控丢弃的违规框数 |
| `duplicate_observations` | int | 被去重丢弃的框数 |
| `vetoed_observations` | int | 因同部位有正类而被否决的框数 |
| `tracked_people` | int | 当前跟踪到的人数 |

### 事件

```cpp
event_type = "logic_ppe_violation"
message    = "人员安全穿戴违规: 未穿工作服"
fields     = { track_id, violation_types, part_states, person_score }
```

同一个人的头部、工装和手套分别记录已上报状态。某类违规确认且当前帧能定位部位时才上报。上报成功后，同一会话内持续出现该类别会去重，随后出现其他类别仍可产生新事件。

---

## 七、平台动作（Web 控制台按钮）

| 动作名 | 作用 |
| --- | --- |
| `ppe_reset_alarm` | 清空全部人员的确认状态与证据（相当于重启逻辑） |
| `ppe_toggle_debug` | 切换左上角调试信息显示（关闭后不绘图，但仍发布变量） |
| `ppe_sensitivity_up` | 运行期把 `violation_min_score` +0.05（钳制 0.50～0.95） |
| `ppe_sensitivity_down` | 运行期把 `violation_min_score` -0.05 |

灵敏度按钮**只改运行期覆盖值**，不改配置文件，重启后回到配置值。注意它只影响 `violation_min_score`，**不影响 `glove_min_score`**。

---

## 八、41 个参数全表

| 参数 | 默认 | 现场值 | 说明 |
| --- | --- | --- | --- |
| `person_labels` | `person` | `person` | 人员类别标签，逗号分隔 |
| `person_min_score` | 0.35 | 0.35 | 人员框最低置信度 |
| `violation_min_score` | 0.75 | 0.75 | 违规框最低置信度（人/帽/工装） |
| `glove_min_score` | 0.5 | 0.5 | **手部专用**门槛 |
| `keypoint_min_score` | 0.3 | 0.3 | 关键点可见性阈值 |
| `roi_name` | 空 | 空 | 作业区域名；留空=整帧；名字不存在则停止判定 |
| `check_helmet` | true | true | 检测未戴安全帽 |
| `no_helmet_labels` | `no_helmet` | **`head`** | 安全帽负类标签 |
| `check_workwear` | true | true | 检测未穿工作服 |
| `no_workwear_labels` | `no_workwear,no_uniform` | 同左 | 工装负类标签（本现场模型无此类） |
| `check_gloves` | true | true | 检测未戴手套 |
| `no_gloves_labels` | `bare_hand,no_glove` | **`hand_bare`** | 手套负类标签 |
| `geometry_gate` | true | true | 部位几何门控总开关 |
| `helmet_top_ratio` | 0.35 | 0.35 | 安全帽框纵向位置上限（相对人高） |
| `helmet_area_min_ratio` | 0.01 | 0.01 | 安全帽框面积 / 人框面积 下限 |
| `helmet_area_max_ratio` | 0.12 | 0.12 | 上限 |
| `helmet_head_ratio` | 0.35 | 0.35 | 安全帽框中心到鼻/眼距离上限 |
| `workwear_top_ratio` | 0.25 | 0.25 | 工装框纵向区间起点 |
| `workwear_bottom_ratio` | 0.75 | 0.75 | 工装框纵向区间终点 |
| `glove_wrist_ratio` | 0.2 | 0.2 | 手套框中心到腕点距离上限 |
| `hand_attach_ratio` | 0.6 | 0.6 | 手框允许伸出人框的最大距离（占人高） |
| `evidence_window_sec` | 4 | 4 | 违规证据时间窗口 |
| `evidence_gap_ms` | 250 | 250 | 独立证据最小间隔 |
| `required_observations` | 3 | 3 | 确认所需观测次数 |
| `required_span_ms` | 1000 | 1000 | 确认所需证据跨度 |
| `reacquire_gap_ms` | 3000 | 3000 | 轨迹断开重置间隔 |
| `state_ttl_sec` | 600 | 600 | 人员状态保留时长 |
| `dedupe_iou` | 0.6 | 0.6 | 同类框去重 IoU 阈值 |
| `require_pose_for_violation` | false | false | 只采信可见部位（需 pose 模型） |
| `positive_veto` | true | true | 正类否决负类 |
| `helmet_positive_labels` | `helmet` | `helmet` | 安全帽正类标签 |
| `workwear_positive_labels` | 空 | **`workwear_on`** | 工装正类标签 |
| `glove_positive_labels` | `glove` | **`hand_gloved`** | 手套正类标签 |
| `helmet_infer_by_absence` | false | **false** | 按"没检测到安全帽"推断未戴 |
| `helmet_search_up_ratio` | 0.6 | 0.6 | 安全帽归属搜索区上扩比例 |
| `helmet_search_side_ratio` | 0.15 | 0.15 | 安全帽归属搜索区左右外扩比例 |
| `workwear_require_positive` | false | **true** | 未穿工装按"缺正类"推断 |
| `workwear_min_person_height` | 80 | 80 | 参与工装推断的最小人体框高 |
| `absence_min_person_score` | 0.6 | 0.6 | 缺正类推断的最小人体框分数 |
| `positive_min_score` | 0.35 | 0.35 | 正类框收集门槛 |
| `glove_require_pose` | false | false | 手套判定是否必须依赖姿态腕点 |

---

## 九、现场当前判定语义（逐项）

| 违规项 | 判定依据 | 生效条件 |
| --- | --- | --- |
| **未戴安全帽** | 检出 `head` 负类框，且归属到某人、通过几何门控、4 秒内 3 次观测 | 看到 `helmet` 正类则否决与撤销 |
| **未穿工作服** | 该人躯干区**可核验**（人高≥80、人框分≥0.6、没被挡>40%）但**没有** `workwear_on` 正类框 | 看到 `workwear_on` 则撤销 |
| **未戴手套** | 检出 `hand_bare` 负类框（分数≥0.5），归属到人（允许伸出人框 0.6×人高） | 看到 `hand_gloved` 则否决与撤销 |
| 遮挡 / 模糊 | 部位 `visible=false` 且无正类框 → `PART_UNKNOWN`（`-`） | **不报警** |

---

## 十、为什么现场这套配置是这样的

现场只有检测模型、没有姿态模型，所有参数都是围绕这个约束定的：

| 参数 | 取值 | 原因 |
| --- | --- | --- |
| `require_pose_for_violation` | false | 开 true 会因"没有关键点"丢掉全部证据 |
| `glove_require_pose` | false | 开 true 会让所有裸手框被判成"不是手" |
| `helmet_infer_by_absence` | false | 08:31 实测：遮挡 + 漏检导致大量误报，改用 `head` 负类直采 |
| `workwear_require_positive` | true | 模型无 `no_workwear`，只能推断 |
| `positive_min_score` | 0.35 | 现场正类框只有 0.38～0.72 |
| `glove_min_score` | 0.5 | 现场手框只有 0.59～0.72 |
| `no_helmet_labels` | `head` | 6 类模型的第 6 类是"露头"，即未戴帽 |

---

## 十一、已知局限与调参建议

### 1. 手部检测最不可靠

现场实测手框只有 24×30 像素，遮挡/远景下会出现框画在头顶的误检。`glove_min_score` 降到 0.5 后这类误检更容易变成报警。

**调参建议**：误报多时优先抬 `glove_min_score` 到 0.6，不要关 `check_gloves`。

### 2. 工装靠"缺正类"推断，天然有误报风险

已加三道门：人框高 ≥ 80、人框分 ≥ 0.6、躯干被挡 >40% 不判。

**调参建议**：如果出现"人蹲着 / 背对 / 被设备挡"的高误报，先抬 `workwear_min_person_height` 和 `absence_min_person_score`，再考虑关项。

### 3. `head` 类必须真能区分"露头"

如果模型把戴帽的人头也框成 `head`，安全帽项会像 08:31 一样翻车。上线后必须专门抽查 `head` 框的准确率。

### 4. 多人重叠时归属可能丢证据

`unique_person_for_box()` 遇到"中心同时落在两个人框内"直接丢弃（`rejected_observations++`）。密集场景下宁可漏报。

### 5. 手套分不清左右手

现场无 pose，所有裸手证据都记为"不分左右手"，确认后两只手徽标一起点亮 `X`，但 `violation_types` 只记一条"未戴手套(未分左右手)"。

---

## 十二、验证方法

### 单元测试（167 条断言）

```bash
cmake -S engine -B build/engine -DBUILD_PPE_TESTS=ON
cmake --build build/engine --target ppe_tests -j4
./build/engine/ppe_tests          # ppe_guard: 167 checks passed
```

覆盖：证据确认/过期/去重、遮挡降级、归属歧义、手部左右判定、几何门控、正类否决、缺正类推断、边缘人与小人跳过、低分正类、撤销机制、手套低门槛等。

### 现场注入验证

视频里没有违规样本时，用注入法把"正类"临时改成不存在的类名，人为制造违规：

```bash
python3 /tmp/inject_glove.py on      # hand_gloved 当裸手 → 应报警
python3 /tmp/inject_glove.py off     # 从 .bak-glove 恢复
python3 /tmp/inject_violation.py on  # workwear_on 当未穿 → 应报警
python3 /tmp/inject_violation.py off
```

注意：必须**同时把正类标签改名**，否则 `positive_veto` 会把同一个框当成正类否决掉，永远测不出报警。

### 上线验收指标建议

- 漏报率 ≤ 2%
- 误报 ≤ 1 次/小时/通道
- 告警延迟 ≤ 3 秒
- 7×24 连续运行 7 天无崩溃
- 推理 ≥ 15 FPS（现场实测 25 FPS，`rknn=15.6ms`，无积压）

---

## 十三、部署步骤

```bash
cd /userdata/rk3588_visual_analysis_framework

# 1) 编译
cmake --build build/engine --target ppe_tests vision_analysis -j4
./build/engine/ppe_tests                     # 必须全绿

# 2) 生成控制台参数目录（参数变了必须重新生成）
python3 tools/build/generate_logics_catalog.py --logic-root projects --output /tmp/logics_new.json

# 3) 部署
cp -f build/engine/vision_analysis /opt/ai_apps/test1025/
cp -f /tmp/logics_new.json /opt/ai_apps/test1025/logics.json
chmod 600 /opt/ai_apps/test1025/logics.json

# 4) 重启
systemctl restart rk3588-app-test1025-ec7b8b987122.service
```

**注意**：旧二进制读到新参数会整份拒绝启动（日志 `[Config] channel 0 logic_parameters validation failed: ... 未知参数`），所以二进制和 `logics.json` 必须一起换。

---

## 十四、性能基线

现场实测（1280×720，单模型）：

```
[Perf][ch00] window=5.03s infer_fps=24.87 mode=single models=1
             avg_success_ms rknn=15.57 worker=18.07 queue+worker=18.22
[Feed][ch00] recv_fps=30.00 throttle_skip=25 enq_fps=25.00
[FrameSync][ch00] lag_frames=0 age_ms=21
```

- 推理 25 FPS 满速，`lag_frames=0` 表示无积压
- 单帧 RKNN 推理 15.6ms，从入库到出结果约 21ms
- 逻辑层开销可忽略（纯内存计算，无 IO）

---

## 十五、版本演进（关键修复记录）

| 版本 | 内容 | 原因 |
| --- | --- | --- |
| v6 | 安全帽搜索区上扩 + 共享信用 | 人框只到肩颈，帽子框在人框上方 → "戴了"被判"没戴" |
| v7 | 关闭 `helmet_infer_by_absence` | 08:31 实测：遮挡+漏检导致批量误报 |
| v8 | `workwear_require_positive` + `glove_require_pose` 降级 | 6 类权重上线，工装无负类 |
| v9 | `positive_min_score=0.35` + 正类撤销机制 | 正类框分数 0.38～0.75，被 0.75 门槛丢弃 → 误判"没穿" |
| v10 | 正类框也算 `PART_CLEAR` | 现场无 pose，只看 `visible` 会永远显示 `-` |
| v11 | `PART_CLEAR` 字符 `.` → `O` | 720p 下 `.` 与 `-` 分不清 |
| v12 | `nearest_person_for_hand()` + `hand_attach_ratio=0.6` | 手伸出人框 → 证据被整条丢弃 → 检出裸手也不报警 |
| v13 | 不分侧裸手确认后点亮 L/R 徽标 | 只亮顶部计数，现场看不出哪一项在报 |
| v14 | `glove_min_score=0.5` | 手框分数 0.59～0.72 卡不住 0.75 → 手套这项等于失效 |
