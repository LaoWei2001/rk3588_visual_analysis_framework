# SDK 接口重构验证记录（2026-11-16）

验证设备：192.168.2.41，RK3588。源码根目录 `/userdata/rk3588_visual_analysis_framework`。

## 功能与构建

- 正式程序和独立图片运行器均编译成功，静态 SDK 不强制 PIC；可由共享库消费者显式开启。
- 20 项 CTest 全部通过，包括公共头隔离、查询、会话、参数、绘图、生命周期、初始化分配失败回滚、
  业务流程、SORT/ByteTrack、ROI、坐标映射、模型状态、帧执行器、数据集规则和采集协议等。
- 调整静态库编译选项后，又通过全部 19 项可执行回归；公共头未变化，其隔离检查沿用上一轮通过结果。
- 单 CPP 分析示例在模拟时间 2000ms 捕获一次驻留报警，并生成标注图。
- 单 CPP 图片推理再次编译运行：两个模型、两张图片，检查各模型阈值及结构体结果。
- 独立视频 CPP 启动四路文件视频、每路加载检测模型；全局回调通过临时字符串名称注册，
  成功读取四路 `person_count`，打印 `global_ready=4`。
- RTSP 客户端解码收到 10 帧；四路分别生成 `raw.jpg`、`annotated.jpg`、`clip.mp4`，
  ffprobe 验证文件有效。正常退出返回 0，事件持久化错误为 0。
- 单 CPP 构建辅助脚本保留 assert：故意失败的断言按预期返回 SIGABRT，而非被 Release 设置删除。
- 部署前后业务逻辑名称列表和图片推理能力协议完全一致；部署后的配置校验通过。

四路输入使用同一测试文件的四个通道，不代表已连接四台实体摄像头；RTSP 是实际输出与接收测试。
GPIO 仅检查接口/链接路径，没有切换设备引脚。离线 LogicSession 的事件捕获不执行真实持久化；
上述图片和录像由 VideoRuntime 的实际事件链路产生。

## 性能对比

相同检测模型、输入视频、两路配置，每次运行 16 秒，丢弃首个 5 秒初始化窗口。
保存重构前的真实二进制进行对照，未更改板端全局 CPU 调频策略。

| 场景 | 旧版 | 新版 |
| --- | ---: | ---: |
| 默认调度，稳定发布帧率 | 每路约 25 FPS | 每路约 25 FPS |
| 限定测试进程使用 CPU 4–7，ABBA 顺序，平均 worker 耗时 | 30.0075 ms | 30.01375 ms |
| 相同 CPU 范围的发布帧率 | 每路 25.00 FPS | 每路 25.00 FPS |
| 绘图基准，关闭性能条，平均每路 | 4.25756 ms | 4.27195 ms |
| 绘图基准，开启性能条，平均每路 | 4.59227 ms | 4.56406 ms |

ABBA 顺序为旧、新、新、旧，各一轮；限定 CPU 仅用于这四个测试进程，没有写入正式运行配置。
受控对照 worker 均值差约 0.021%；绘图均值变化小于 1%。
默认动态调频/迁核下，旧版均值约 31.15ms，新版约 31.54ms；旧版自身同通道不同轮次
后处理也从约 1.56ms 波动到 2.30ms。不能把默认调度的短时均值差直接归因于接口重构。
在已测场景没有观察到稳定的代码性能回退，有限样本不构成所有模型、路数和负载绝对零开销的证明。
所有轮次原始数据均保留，避免只展示最优样本。

## 文件和部署

完整原始记录、配置、旧二进制、脚本和事件证据保存在：

`/userdata/rk3588_visual_analysis_framework/build/sdk-acceptance/20261116`

其中 `comparison.json` 是全部性能轮次汇总；`runtime-global-acceptance/verification.json` 是
四路事件文件和 RTSP 验证结果。`rk-sdk-final-ctest.log`、`rk-sdk-static-ctest.log` 记录最终回归。

已更新部署文件：

- `/opt/ai_apps/test1115/vision_analysis`
  - SHA256：`b6622d3a3a38e51c27151443c67ae1be60c56febcf42b980565042e255c8eefd`
- `/opt/ai_apps/_console/inference_runtime/image_inference_runner`
  - SHA256：`62a846240f69597611547d84ab611db410ca9ec84b081e6a942d0a0a21dda8ab`

旧文件备份目录：`/userdata/rk3588_visual_analysis_framework/build/deploy-backups/sdk-refactor-20261116-001746`。

使用入口见 [SDK 独立程序指南](standalone-cpp.md) 和 [视频运行时指南](video-runtime.md)。
