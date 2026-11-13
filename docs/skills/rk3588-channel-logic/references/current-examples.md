# 当前通道模块索引

下表按 2026-11-13 的 `projects/modules/` 与注册宏核对，共 28 个通道模块，用于选择真实存在的参考实现。模块集合会变化；
使用前仍应运行 catalog 校验并同时阅读该目录的 `logic.cpp` 与 `logic.json`。

| logic ID | 当前用途 | 参考注意事项 |
|---|---|---|
| `logic_default` | 空操作模板 | 最小注册与 manifest 结构 |
| `logic_course_01` | 文字、圆形绘制 | 课程代码，不是业务告警 |
| `logic_course_02` | 时间、上下文和参数 | 对照 manifest 的 `test_string/test_num` |
| `logic_course_03` | 配置、模型和推理结果读取 | 用于认识 `AlgoResult` |
| `logic_course_04` | ROI 信息读取 | 学习示例遍历 ROI，但直接访问每个 `polygon[0]`；生产代码必须先检查上下文、区域指针、顶点数量和有效性 |
| `logic_course_05` | 按名称查 ROI、检测框命中和框颜色 | 参数 `roi_name` 必须与运行配置一致 |
| `logic_course_06` | 每通道跨帧 state | 演示调用计数 |
| `logic_course_07` | 自定义 Action | `increment/decrement/change` |
| `logic_course_08` | 定时间隔创建图片事件 | 当前事件 ID 为 `logic_course_08`，带模块模板与上报字段 |
| `logic_course_09` | 空骨架 | 标签描述按钮上报，但当前 C++/manifest 未实现该功能 |
| `logic_course_10` | 空骨架 | 标签描述吊钩案例，但当前没有案例实现 |
| `logic_course_gpio` | 检测标签触发 GPIO 输出 | 涉及真实硬件电平，先核对引脚和有效电平 |
| `logic_dify` | Dify 周期截图和自定义变量 | 当前较完整的周期事件、JSON 字段、模板示例 |
| `logic_roi_person_count_demo` | 发布 ROI 内 `person_count` | 是当前 `global_person_count_alarm_demo` 的上游 |
| `logic_crane_motion`、`logic_crane_hook` | 发布行车运动与吊钩状态 | 行车全局控制器输入，依赖对应模型标签和参数 |
| `logic_crane_intrusion`、`logic_crane_helmet` | 发布入侵与安全帽状态 | 行车全局控制器输入，涉及业务 ROI/状态机 |
| `logic_relay` | Action 切换继电器 | 有真实 GPIO 副作用 |
| `logic_default1019` | 输出每帧编号 | 目录为 `logic_1019/`，ID 以注册宏为准；高频日志示例 |
| `logic_roi_dwell_demo` | 命名 ROI 持续占用报警 | 公开 SDK、状态、outputs 与事件组合示例 |
| `logic_multi_model` | 多模型配置的空业务入口 | 当前回调为空，不能当作已实现多模型业务规则的例子 |
| `logic_depth_demo` | 米制深度和目标距离查询 | 与 `yolo26_depth` 的单位语义匹配 |
| `logic_zip_depth_demo` | 相对逆深度查询 | 输出不表示米制距离 |
| `logic_crane_hook_calibration` | 手动采集吊钩标定样本 | 当前引用 `logic_crane_hook` 的几何实现，不能独立删除后者 |
| `logic_ppe_guard` | 部位级穿戴状态编排和告警 | 独立 guard 状态机、参数、Action 与事件 |
| `logic_ppe_compliance` | 作业人员穿戴检测 | 使用自身标签、确认窗口和事件声明，不能与其他 PPE 模块混用参数 |
| `logic_ppe_yolov8s_v2` | 三类装备的疑似缺失检查 | 只把几何推断标作疑似缺失，不声称真实身体可见性 |

`logic_crane_helmet` 的默认 `missing_helmet` 模式采用裸头证据优先：匹配裸头的人员不受大小限制；
没有匹配裸头或安全帽时，人员框宽度达到 `person_min_width` 且高度达到 `person_min_height`
才参与违规确认。两个阈值默认均为固定640×640业务坐标下的40像素，不使用面积阈值，也不按原始
视频或模型输入分辨率换算。任一边不足均过滤，等于阈值有效，0仅关闭对应方向的过滤。
`bare_head` 模式只使用裸头证据。两者保留 ROI、确认/解除时间以及全局静止场景互锁。

当前没有注册 `logic_path_sop`、`logic_periodic_snapshot_demo` 或 `logic_upload_teach`，Web 也不会为这些
历史模块生成配置。文档不得把历史模块写成现成功能。

查询当前清单：

```bash
python3 tools/build/generate_logics_catalog.py --check
./vision_analysis --list-logics   # 仅在已有当前源码构建出的二进制时
```
