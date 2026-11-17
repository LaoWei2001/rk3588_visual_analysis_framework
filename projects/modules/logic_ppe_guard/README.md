# logic_ppe_guard — 安全穿戴编排（部位级三态）

面向"作业现场没戴安全帽 / 没穿工作服 / 没戴手套就报警，同一个人不重复报警"的编排模块。
与 `logic_ppe_compliance`（v1）共用事件类型 `logic_ppe_violation`，区别在于 v1 只按
"人员框 + 违规框 + 多帧计数"，本模块增加了**部位可见性门控、左右手分侧、归属唯一性判定
和会话重置**。

## 一、编排链路

```
人员检测（含 track_id）
  → 姿态关键点（可选）→ 部位可见性
  → 违规目标框 → 唯一人员框 → 部位归属（含左右手）
  → 每部位滑动窗口证据 → 达到"次数 + 跨度"才确认
  → 按人合并：同一 track_id 只提交一次事件
```

四条落地判定：

1. **看不到装备 ≠ 没穿戴**。部位不可核验（关键点不可见、没有姿态结果、归属不明）时记为
   `unknown`，该帧不写违规证据，也**不清空**已有证据；证据靠窗口自然过期。
2. **检测到装备 ≠ 属于这个人**。违规框中心必须落在**恰好一个**人员框内；同时落在两个
   人员框内一律丢弃（记 `unknown`），不按面积或距离猜归属。
3. **左右手分开**。有 COCO17 姿态时，裸手框按腕点距离分侧；两腕距离接近（差值 <
   0.35×腕间距）时不猜，只计一次"不分侧"手套证据，只影响"手套"这一项，不点亮某一只手。
4. **同一会话不重复报警**。按 `track_id` 维护状态，`reported` 置位后不再提交；同一
   `track_id` 消失超过 `reacquire_gap_ms` 再出现，视为**新的一次作业会话**，重新积累证据。

## 二、部位与关键点映射（COCO17）

| 部位 | 可见性依据 | 违规标签参数 |
| --- | --- | --- |
| 安全帽 `H` | 鼻(0) 或 双眼(1,2) 同时可见 | `no_helmet_labels` |
| 工作服 `T` | 双肩(5,6) 且 至少一髋(11,12) | `no_workwear_labels` |
| 左手套 `L` | 左腕(9) | `no_gloves_labels` |
| 右手套 `R` | 右腕(10) | `no_gloves_labels` |

关键点分数低于 `keypoint_min_score` 视为不可见。**模型推测出的被遮挡关键点不能当证据**，
因此宁可整帧记 `unknown`。

## 三、部位状态（画面上的字符）

| 字符 | 含义 | 是否触发报警 |
| --- | --- | --- |
| `-` | `unknown`：遮挡、模糊、归属不明、该项未启用，或本帧两者都没看到 | 否 |
| `O` | `clear`：本帧**看到了「戴好了 / 穿好了」的正类框**，或有关键点看到该部位 | 否 |
| `?` | `candidate`：有违规证据，未达确认门槛 | 否 |
| `X` | `violation`：已达确认门槛 | 是 |

> 早期版本 `clear` 输出 `.`，在 720p 现场画面上和小 `-` 几乎分不清，已改为 `O`（ok）。
>
> `clear` 的含义是**正向确认**：本帧真的看到了 `helmet` / `workwear_on` / `hand_gloved` 框
> （由 `positive_veto` + `positive_min_score` 收集），不再是"没看到违规"。

示例（现场 6 类权重实测）：`#2 HO TO L- R-` = 帽子已戴、工装已穿、双手未判定；
注入一次"工装正类永远不匹配"后变成 `#2 HO TX L- R-` 并转红报警。

## 四、参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `person_labels` | `person` | 人员类别，逗号分隔 |
| `person_min_score` | 0.35 | 人员框最低置信度 |
| `violation_min_score` | 0.75 | 违规框最低置信度；低于它一律不采信（现场要求 0.75） |
| `glove_min_score` | 0.5 | **手部专用**门槛。手框天生小、分数低（现场实测 0.59~0.72），沿用 0.75 会让“未戴手套”永远不触发；只对手部生效，人/安全帽/工装仍走 `violation_min_score` |
| `keypoint_min_score` | 0.3 | 关键点可见性阈值 |
| `roi_name` | 空 | 只判定框中心落在该 ROI 的人员；**留空=整帧**；区域名不存在时停止判定 |
| `check_helmet` / `check_workwear` / `check_gloves` | true | 三项独立开关 |
| `no_helmet_labels` | `no_helmet` | **只能填负类**。`bare_head`/`head` 是中性“头”类，填进来会让戴帽也报警 |
| `no_workwear_labels` | `no_workwear,no_uniform` | 模型明确输出的未穿工作服类别 |
| `no_gloves_labels` | `bare_hand,no_glove` | 模型明确输出的裸手类别 |
| `evidence_window_sec` | 4 | 证据窗口 |
| `evidence_gap_ms` | 250 | 两条独立证据的最小间隔，防相邻帧重复计数 |
| `required_observations` | 3 | 确认所需次数 |
| `required_span_ms` | 1000 | 证据最小跨度 |
| `reacquire_gap_ms` | 3000 | 轨迹断开多久算新会话 |
| `state_ttl_sec` | 600 | 人员状态保留时长，超时清理防内存增长 |
| `geometry_gate` | true | 部位几何门控总开关，关闭仅供排障对比 |
| `helmet_top_ratio` | 0.35 | 安全帽框中心允许的最高位置（占人框高） |
| `helmet_area_min_ratio` / `helmet_area_max_ratio` | 0.01 / 0.12 | 安全帽框 / 人框 面积比区间 |
| `helmet_head_ratio` | 0.35 | 有姿态时，安全帽框中心到鼻/眼的距离上限（占人框高） |
| `workwear_top_ratio` / `workwear_bottom_ratio` | 0.25 / 0.75 | 工作服框中心允许的垂直区间 |
| `glove_wrist_ratio` | 0.2 | 手套框中心到对应腕点的距离上限（占人框高） |

输出：`ppe_alarm`、`confirmed_people`、`candidate_observations`、`tracked_people`。
上报字段：`track_id`、`violation_types`、`part_states`、`person_score`。

## 五、对模型与机位的要求（本模块无法代劳）

- 需要**人体检测 + track_id**；姿态模型（`yolov8_pose` / `yolo26_pose`）用于可见性门控，
  没有也能跑，但所有部位都会停在 `unknown`，只剩"显式违规标签"这一条路径。
- 必须能直接输出**违规类别**（未戴帽 / 裸手 / 非工服）。只输出"安全帽 / 工服"正类时，
  "没检测到"不能当违规，需要补负样本类别，否则该部位只能一直是 `unknown`。
- 手套小、动作快：需要原图局部裁剪识别，建议提高该通道推理分辨率或增加抽帧后的局部复查。
- 现有画面里控制柜长期挡住躯干和双手、三人挤在平台上互相重叠——单机位盲区下的
  `unknown` 占比会很高，这是物理限制，应补入口/侧前方机位，而不是把 `unknown` 改判违规。

## 六、需要现场验证的边界

- 纯视觉**无法**跨时段识别同一个人：`track_id` 切换会新建会话并再次报警。
- 三人重叠时若只有两个人体框，第三个人不会进入判定（不靠头部单独补人，除非另配头部模型
  并从 `person_labels` 加入）。
- 两人换位后若跟踪 ID 互换，装备状态会跟着 ID 走，属已知的错人归属风险。
- 阈值是初始值，需按现场误报/漏报与报警延迟实测调整。

## 七、离线回归测试

状态机与证据窗口已抽成不依赖 `ChannelContext` 的纯逻辑（`ppe_guard.h` / `ppe_guard.cpp`），
因此可以在没有相机、没有模型的情况下跑回归：

```bash
cmake -S engine -B build/engine -DBUILD_PPE_TESTS=ON
cmake --build build/engine --target ppe_tests -j4
./build/engine/ppe_tests          # 或 cd build/engine && ctest -R ppe_regression
```

正常输出 `ppe_guard: 65 checks passed`，退出码 0。

覆盖的 15 个场景：确认需同时满足次数与 1 秒跨度；相邻帧去重（不能靠连续三帧凑确认）；
窗口外旧证据过期；无姿态时全 `unknown`；部位可见且未见违规为 `clear`；部分关键点遮挡只降级该部位；
违规框同时落在两人框内时丢弃；裸手框按腕点分左右手；两腕接近时不猜左右手只记"不分侧"证据；
`reacquire_gap_ms` 后视为新会话可再次报警；同帧重复 `track_id` 只算一人；时间回退清空时间基准；
关闭的检查项不产生候选；低置信度过滤与标签精确匹配；`state_ttl_sec` 清理长期不出现的人员。

## 八、部位几何门控（挡“类别对了、长错地方”）

模型会把戴着头盔的头判成 `bare_head`，也会把戴着的手套判成别的类别。**多帧确认对这类偏差无效**：同一个位置稳定误检，25fps 下 4 秒窗口轻松凑够 3 次观测和 1 秒跨度。所以证据在入账前先过一道物理约束：

| 部位 | 几何条件 | 不满足时 |
| --- | --- | --- |
| `H` 安全帽 | 框中心在人框上部 `helmet_top_ratio`(0.35) 内；面积占人框 `1%~12%`；有鼻/眼关键点时，框中心到最近的鼻/眼 ≤ `0.35×人高` | 丢弃，`rejected_observations` +1 |
| `T` 工作服 | 框中心落在人框 `25%~75%` 高度区间 | 丢弃 |
| `L`/`R` 手套 | 框中心到对应腕点 ≤ `0.2×人高`，并沿用“两腕距离接近则不猜左右手”的分侧规则 | 超距离 → 丢弃；只有腕点缺失或左右模糊时才记“不分侧”证据 |

四条刻意保留的宽松处，避免门控把真违规也吃掉：

1. **安全帽框可以高过人员框上边缘**：人体检测框常常从颈肩开始，头部框的中心天然落在框外。安全帽项把人员框向上扩展 `helmet_top_ratio` 个人高再判归属，垂直窗口取 `[-0.35, +0.35]`，水平范围不动，避免跨人归属。
2. **没有姿态结果时**，安全帽只靠“位置 + 面积”两条约束，不会因为拿不到关键点就整条丢弃。
3. **腕点全丢时手套项直接丢弃**——没有腕点就无法确认“这是手”，这时报警的依据只剩类别名，等同于猜。**没有姿态模型的通道，手套项不会报警**。
4. **不跨部位重判**：框被 `H` 的几何否掉不会顺手记成 `T`/`L`/`R`，宁可这帧没有证据。

留一个诚实的边界：**位置和面积都合理、但类别判错**的框（工人戴着安全帽，模型却说 `bare_head`）几何门控管不了，那只能换模型或加“正类否决”（模型输出 `helmet` 正类时丢弃重叠的 `bare_head`）。`engine/tests/ppe/test_ppe_guard.cpp` 里的 `test_helmet_geometry_keeps_correct_position_box` 就是把这个边界固定下来的用例。

排障用法：画面顶部汇总行第四项 `几何丢弃: N` 与输出 `rejected_observations` 直接说明“模型给的框有多大比例根本长不对地方”。

- `几何丢弃` 持续很大 → 模型或机位问题（类别定义不对、镜头俯角太小导致人框压扁），调参数没用，先换标签或改机位。
- `几何丢弃` 正常但仍有误报 → 看部位状态是 `?`（未确认）还是 `X`（已确认），后者说明同类误检在时间上稳定，需要收紧对应部位的关键点/面积阈值，或直接怀疑模型类别。

## 九、一个真实踩过的坑：中性“头”类不能当“未戴帽”

现场出现过：工人戴着蓝色安全帽，模型在头部输出了 `bare_head`，编排就判成了未戴帽并报警。

这不是模型错，也不是几何门控能救的——**框的位置、大小完全合理**，错在配置：把 `bare_head` 这个中性的“头部”类当成了“未戴帽”违规类。

判别标准很简单：某个类别在**合规人员身上也会稳定出现**，它就不是违规类。中性类只能当“部位存在”的信号，不能当“该部位没戴装备”的证据。

后果与应对：

- 配了中性类 → 所有人都会“违规”，而且多帧确认只会让它更坚定。
- 模型**没有**负类（只有人 + 头）时，安全帽项就只能停在 `unknown`：这是诚实的结果，不要拿中性类凑合。
- 若模型同时输出正类（如 `helmet`）与负类（如 `no_helmet`），本模块用“正类否决”处理：同一部位上有正类框就丢弃负类框（见第十节第 6 条）。

## 十、报警准入条件（一次报警要同时满足）

1. **类别对得上**：框的类别名必须和 `no_helmet_labels` / `no_workwear_labels` / `no_gloves_labels` 里写的字面量一致。模型换了 `label_path` 没换时，画面底部那行 `标签: person3 helmet1` 会直接暴露真实的类别名。
2. **置信度够**：`score >= violation_min_score`，默认 `0.75`。
3. **是同一个物品**：同类框之间 IoU `>= dedupe_iou`（默认 `0.6`）时只留最高分的那一个，同一个物品上的多个框不会凑成多条证据。
4. **归属唯一**：框必须落在恰好一个人员框内；落在两个人身上、或不在任何人身上，一律丢弃。
5. **部位位置合理**：见第八节的几何门控。
6. **没有正类否决**：同一部位上如果识别到 `helmet` / `glove` 这类“穿好了”的正类框，本帧的负类框直接丢弃（`positive_veto`，默认开）。这条专治“戴着却报没戴”。
7. **部位可见**（可选）：打开 `require_pose_for_violation` 后，该部位关键点不可见（遮挡、出画、模糊）就不采信证据——宁可漏报，不误报。**没有姿态模型的通道打开它会永远不报警**，这是设计取舍，不是 bug。
8. **多帧确认**：证据数 `>= required_observations` 且时间跨度 `>= required_span_ms`。

新增参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `dedupe_iou` | 0.6 | 同类框去重阈值，`0` 表示关闭 |
| `positive_veto` | true | 正类否决负类，总开关 |
| `helmet_positive_labels` | `helmet` | 安全帽正类标签，留空则不做否决 |
| `workwear_positive_labels` | 空 | 工作服正类标签，留空则不做否决 |
| `glove_positive_labels` | `glove` | 手套正类标签，留空则不做否决 |
| `require_pose_for_violation` | false | 只采信可见部位；需要通道输出人体姿态模型 |

新增输出：`duplicate_observations`（本帧被同类去重丢弃的框数）、`vetoed_observations`（本帧因正类否决丢弃的负类框数）。画面左上角第二行 `丢弃 / 去重 / 否决` 三个数就是它们。

排障口诀：

- `去重` 一直很大 → 模型在同一个物体上重复出框，先调 `dedupe_iou`（0.5 更激进）或换 NMS 阈值。
- `否决` 一直很大 → 模型同时给出正负两类，说明类别定义含糊，应该只保留负类，或把这些正类从违规标签里删掉。
- 底部 `标签:` 行里出现的类别名和 `person_labels` 完全对不上 → **模型与标签文件不匹配**，此时编排做什么都没用，先修 `label_path`。

## 十一、当前部署权重（yolov5s_relu.rknn）只有三类

对 `assets/yolov5s_relu.rknn` 逐帧解码核对（输出 `(1,24,80,80)/(1,24,40,40)/(1,24,20,20)`，
`24 = 3 anchors × (5 + 3 classes)`），类别顺序与画面框一一对应：

| class | 名称 | 说明 |
|---|---|---|
| 0 | `person` | 人 |
| 1 | `helmet` | 安全帽（**正类**，只有戴着的才出框） |
| 2 | `hand` | 手/手套（**正类**，框是手本身，不区分戴没戴） |

因此，标签文件必须是 `assets/labels_ppe.txt`（person / helmet / hand）。
若仍挂 `labels_helmet.txt`（person / helmet / bare_head），class2 的“手”就会被读成
`bare_head`，再被当成“未戴安全帽”的证据 —— 这就是“戴着手套却报没戴安全帽”的根因。

由此有三条硬结论：

1. **未戴安全帽**：模型没有负类，只能靠 `helmet_infer_by_absence`（头部区域可核验、
   无安全帽框、且人框自身分数 ≥ `violation_min_score`）推断。打开它才可能报警；
   关掉它（默认）时安全帽项永远停在未知，不报警。
2. **未穿工作服**：该权重没有工作服类，`check_workwear` 必须关掉（开了也只会一直未知）。
3. **未戴手套**：该权重只有“手”这一个正类，没有“裸手/手套”负类，同样判不了，
   `check_gloves` 必须关掉。要做手套，需要重训 ≥4 类模型
   （如 `person / helmet / hand_gloved / hand_bare`），并保证负类真的只在裸手时出现。

配套的现场配置（`assets/depth_multi_model.json`）已按上表改好：
`label_path = assets/labels_ppe.txt`、`violation_min_score = 0.75`、
`helmet_infer_by_absence = true`、`no_helmet_labels = ""`、
`check_workwear = false`、`check_gloves = false`。

## 十二、平台编排接入（Web 控制台步骤）

模块在控制台里表现为一个“逻辑节点”，参数面板读 `logic.json`，按钮读 `actions`，事件类型读 `event_types`。
接入一条通道的顺序：

1. **建通道**：视频源（file / rtsp），确认 `10_23.mp4` 能出图。
2. **挂模型**：`yolov5s_relu.rknn` + `label_path = assets/labels_ppe.txt`。
   `obj_thresh` 建议 0.30~0.35（低分框由逻辑层的 0.75 门槛统一拦截），`nms_thresh` 0.45。
3. **选逻辑**：`logic_ppe_guard`，label 是「安全穿戴编排（部位级三态）」。
4. **填参数**：必改四项——
   - `person_labels = person`（与模型类名一致）
   - `violation_min_score = 0.75`（现场验收门槛）
   - `helmet_positive_labels = helmet`、`helmet_infer_by_absence = false`（现权重判不了“没戴”，见第十三节）
   - `check_workwear = false`、`check_gloves = false`（当前权重判不了，开了只会一直未知）
5. **画 ROI（可选）**：控制台画一个多边形，把 `roi_name` 填成同名；留空表示整帧。
6. **配上报**：`report_policy.enabled = true` 并添加投递地址；事件类型选 `logic_ppe_violation`。
   投递内容建议带 `track_id / violation_types / part_states / person_score` 四个字段。
7. **看输出**：`ppe_alarm`（总报警）、`confirmed_people`、`candidate_observations`、`tracked_people`，
   以及排障用的 `rejected_observations`（几何丢弃）、`duplicate_observations`（同类去重）、`vetoed_observations`（正类否决）。

### 画面上的按钮

| 按钮 | action id | 作用 |
|---|---|---|
| 复位穿戴告警 | `ppe_reset_alarm` | 清空所有人员的确认状态与证据队列（违规若仍在，会重新累计） |
| 灵敏度 + | `ppe_sensitivity_up` | 报警置信度门槛 +0.05（上限 0.95），画面上显示当前值 |
| 灵敏度 - | `ppe_sensitivity_down` | 门槛 -0.05（下限 0.50） |
| 调试信息开关 | `ppe_toggle_debug` | 隐藏/显示左上角候选、丢弃、灵敏度文字；检测框与人号徽标保留 |

灵敏度按钮只改运行期覆盖值，不改配置文件；重启后回到 `violation_min_score`。

### 上线前自检清单

- [ ] 底部标签统计行能同时看到 `person` 和人头上的 `helmet`；看不到说明 `label_path` 或 `person_labels` 对不上。
- [ ] 戴帽工人连续 10 秒内 `H` 不出现 `X`；若出现，先看是不是 `helmet_infer_by_absence` 叠上模型漏检。
- [ ] 摘掉安全帽后 2~4 秒内 `H` 变 `X` 且 `ppe_alarm = true`。
- [ ] 同一人只出现一条事件；`duplicate_observations` 长期不为 0 说明模型同类重复框多，可调 `dedupe_iou`。

## 十三、为什么“没看到安全帽框”不能推断“没戴”（08:31 实测）

2026-08-27 08:31 的现场画面里两名工人都戴着蓝色安全帽，模型也检测出了两个 `helmet` 框，
但 `helmet_infer_by_absence = true` 时两人都被判 `H=X` 并报警。逐帧核对后的三个原因：

1. **人体框只框到肩颈**，帽子框中心在人体框顶之上；严格 containment 判失败就把“戴了”记成“没戴”。
   已修：安全帽归属改成“人体框上抬 `helmet_search_up_ratio`、左右外扩 `helmet_search_side_ratio` 的搜索区”，
   且搜索区内有安全帽框的人**共享信用**（多人挨着站时不搞“最近者独占”）。
2. **遮挡来自设备而不是人**。工人站在护栏平台上，头顶后面就是机架横梁；头部被结构件挡住时，
   没有安全帽框和“真没戴”在单帧里长得一模一样。本模块只能排除“被另一个人挡住”（人框重叠），
   排除不了设备遮挡——那需要头部关键点或头部检测框来证明“头看得见”。
3. **检测器会漏**。同一顶帽子在连续帧里偶尔整帧消失，`required_observations=3` / `required_span_ms=1000`
   在 25fps 下 1 秒就能凑够证据，多帧确认挡不住这种系统性缺失。

结论：**缺帽推断默认关闭**。它带来的误报（对着戴帽的人报警）比漏报更伤现场信任，
而且无法用阈值调优消除——这是“没有负类”的模型能力问题，不是参数问题。

要真正判“未戴安全帽”，只有两条路：

- **重训一个带负类的模型**：`person / helmet / no_helmet`（再扩两类成 `hand_gloved / hand_bare`、`workwear_on / workwear_off`），
  把负类的 `no_helmet` 填进 `no_helmet_labels`，`helmet_infer_by_absence` 保持 false。
  这是唯一能同时判“安全帽 / 工作服 / 手套”的路线。
- **加头部可核验信号**：接一个头部/人脸检测或 COCO17 姿态模型，只有“头部确实可见且没有安全帽框”才推断，
  并且仍然建议同时训练负类作为主判据。

在模型补齐之前，本通道的正确行为是：**不报警，人号徽标显示 `H-`（未知）**，而不是猜。


## 十四、六类权重下的判定规则（2026-11-16 上线）

现场换成了 6 类权重 `assets/best.rknn`（输入 640×640、输出 33=3×(5+6)），标签顺序：

```
0 person / 1 helmet / 2 hand_gloved / 3 hand_bare / 4 workwear_on / 5 head
```

注意第 6 类是中性 **`head`（头部）**，不是 `no_helmet`。所以：

| 部位 | 正类 | 负类 | 规则 |
| --- | --- | --- | --- |
| 安全帽 `H` | `helmet` | `head` | 看到 `head` 且同一人没有 `helmet` → 违规；两者都没看到 → 未知不报 |
| 工装 `T` | `workwear_on` | 无 | **`workwear_require_positive=true`**：人框够高、躯干未被别人挡住、本帧没有 `workwear_on` → 违规 |
| 手套 `L`/`R` | `hand_gloved` | `hand_bare` | 看到 `hand_bare` 就是证据；模型区分左右手时按腕点分侧，分不清走"不分侧"通道 |

用户口径（m02112）："检测到人，并且人得穿工装才不会报警，裸手识别到了就报警。"

### 关键参数（现场 `assets/depth_multi_model.json` 的 `logic_parameters`）

```json
{
  "no_helmet_labels": "head",
  "helmet_positive_labels": "helmet",
  "workwear_positive_labels": "workwear_on",
  "no_gloves_labels": "hand_bare",
  "glove_positive_labels": "hand_gloved",
  "workwear_require_positive": true,
  "workwear_min_person_height": 80,
  "absence_min_person_score": 0.6,
  "positive_min_score": 0.35,
  "glove_require_pose": false,
  "helmet_infer_by_absence": false
}
```

### 正类撤销机制（防"漏检一帧就永久挂红"）

检测器漏检一两帧是常态。`Guard::update` 每帧先检查 `positive_seen[part]`，一旦本帧
看到了 "戴好了/穿好了" 的正类框，就**清空该部位已确认的违规并允许重新上报**：

```cpp
if (person.positive_seen[part] && (evidence.confirmed || !evidence.violations.empty())) {
    evidence.violations.clear();
    evidence.confirmed = false;
    person_state.reported = false;
}
```

### 端到端验证记录

现场源是循环播放的 `assets/10_23.mp4`（201 秒，08:29:04→08:31:44）。整段视频里的人
**全部戴蓝安全帽、穿蓝工装**，所以只能验证"不误报"，验证"能报"需要人为注入：

| 场景 | 顶部计数 | 人号徽标 | 结论 |
| --- | --- | --- | --- |
| 正常配置 | `候选0 确认0 人员1` | `#2 HO TO L- R-`（绿框） | 不误报 ✅ |
| 注入 `workwear_positive_labels=workwear_disabled_for_test` | `候选1 确认1 人员1`（红字） | `#2 HO TX L- R-`（红框） | 约 1 秒后确认报警 ✅ |
| 恢复配置 | `候选0 确认0 人员1` | `#2 HO TO L- R-`（绿框） | 恢复正常 ✅ |

注入脚本 `inject_violation.py on|off`（临时把 `workwear_positive_labels` 改成一个永不存在的类名，
等价于"所有人身上都没检出工装正类"，用来打通往"缺正类 → 报警"整条链路；`off` 从 `.bak-inject` 还原）。

### 已知局限

1. **手套依赖模型区分左右手**。`hand_bare` 若带 `track_id` 且能对上人，会按腕点分侧；
   无姿态、无 track 时两次腕距接近，只计"不分侧"证据，画面显示 `L- R-` 但内部会累计
   `gloves_unsided`。现场实测手部框只有 24×30 像素，远景基本不可靠。
2. **工装判定靠"缺正类"**，仍属推断。已加三道门：人框高 ≥ `workwear_min_person_height`、
   人框分 ≥ `absence_min_person_score`、躯干被别人框盖 >40% 时 `torso_checkable=false` 不判。
   如果现场出现"人蹲着/背对/被设备挡"的高误报，优先抬 `workwear_min_person_height` 和
   `absence_min_person_score`，而不是关掉整项。
3. **`head` 类必须真能分出发"露头未戴帽"**。如果模型把戴帽的人头也框成 `head`，
   就会和 08:31 的缺帽误报一样翻车。上线后要专门抽查 `head` 框的准确率。
4. **手套用 0.5、其余用 0.75 是刻意的不对称门槛**（2026-11-16 现场决定）。原因是手框
   实测只有 0.59~0.72，卡 0.75 等于整项失效；代价是手部误检更容易变成报警。
   手部本就是六类里最不可靠的一项（实测有框画在头顶的误检），
   误报压不住时优先抬 `glove_min_score` 到 0.6，而不是关掉整项。
