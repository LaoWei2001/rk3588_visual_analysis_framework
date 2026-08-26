# 当前通道模块索引

下表来自当前 `vision_analysis/src/logic/modules/`，用于选择真实存在的参考实现。由于用户可能增加或删除某些模块，所以模块集合会变化；
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
| `logic_global_input_demo` | 发布 `target_count/local_alarm/risk_ratio` | 是当前全局聚合 demo 的上游 |
| `logic_relay` | Action 切换继电器 | 有真实 GPIO 副作用 |

当前没有注册 `logic_path_sop`、`logic_periodic_snapshot_demo` 或 `logic_upload_teach`。Web 编辑器中
仍保留 SOP 节点并会生成 `logic_path_sop`，但仓库当前没有对应 C++ 模块；在模块补回且 catalog
校验通过之前，SOP 画布不能当作可运行配置。文档不得把历史模块写成现成功能。

查询当前清单：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
./vision_analysis --list-logics   # 仅在已有当前源码构建出的二进制时
```
