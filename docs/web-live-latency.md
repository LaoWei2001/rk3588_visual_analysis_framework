# Web 实时画面延迟修复（2026-10-26）

## 原因与修改

本地显示直接消费引擎的预览画面；Web 多经过 RTSP 编码、FFmpeg 封装、HTTP 传输及浏览器 MSE 解码/播放。两者的缓冲不能用同一个指标判断。

本次复核发现，原网页初始保留 0.6 秒数据，发生 starvation 后每次增加 0.4 秒，最大到 1.8 秒，后续没有回落机制。小幅积压只在超过约 1.2–2.4 秒时用 1.05 倍速追赶，超过 3 秒才跳转。短时卡顿后仍持续显示旧画面，与用户报告的“开始正常、卡一次后落后 2–3 秒”吻合。

当前策略：

- 初始/常态目标缓冲 0.3 秒；临时恢复缓冲每次增加 0.15 秒，上限 0.6 秒。
- 连续呈现帧稳定 5 秒后，每 2 秒降低 0.1 秒，回到常态。暂停、seek、后台和真实停帧会重置稳定计时。
- 超过目标 0.15 秒时以 1.08 倍速消化积压，超过目标 0.4 秒时使用 1.12 倍；落后最新可用区间超过 1 秒时跳回区间末尾附近。正常抖动留有死区，避免逐片 seek。
- 恢复播放时立即同步连续缓冲区；短时抖动不关闭 HTTP、不重建解码器。断流、超时、配额回收、重试退避仍保留。
- FFmpeg 保持 H.264 stream copy，分片时长由 200 ms 改为 100 ms；对引擎本地 RTSP 输出，将启动探测限制为 analyzeduration=100000 / probesize=65536。

推理、模型、NPU 调度、视频输入分辨率与分析线程数没有修改。追帧可能跳过已过期的预览画面，不能把每帧必展示作为低延迟保证；源端、网络或客户端持续处理不过来时仍可能卡顿。

## 可复现验证

新增 [时间码测试](../web_console/backend/tests/test_live_latency.py)：320×180@30 的合成源在每帧编码生产者的单调毫秒数，经过 GStreamer RTSP、实际后端路由、H.264 fMP4 和真实 Chromium，再从解码图像读取时间码。服务与浏览器用最小 RTT 样本校准时钟。不是仅用 buffered.end-currentTime 冒充端到端延迟。

相同条件下，每次人为阻塞 HTTP 发送 1.1 秒，连续注入三次；随后等待 10 秒并采样 50 次。基线和候选分别构建，均只建立一次播放连接。

| 指标 | 修改前 | 修改后 |
| --- | ---: | ---: |
| 停顿恢复后帧龄 P50 | 1498.50 ms | 510.29 ms |
| 停顿恢复后帧龄 P95 | 1516.85 ms | 527.84 ms |
| 页面到首批呈现帧 | 2.55 s | 2.36 s |
| HTTP 视频连接数 | 1 | 1 |

这是本机合成源到浏览器解码画面的单次 A/B 测量；CPU 频率和后台负载未锁定。不包含现场摄像头和物理显示器，也不承诺所有网络/客户端达到这些数值。

其他验证：

- 前端缓冲/恢复单元回归：7 项通过。
- 后端流封装、独立会话、慢客户端及共享生产者回归：23 项通过。
- 真实浏览器日志负载、切回页面、断流、配额压力、缓冲超时与重试：5 项通过。
- 两个独立 Chromium 播放 1280×1280@30 合成 RTSP，跨刷新、持续播放及关闭其中一个窗口：通过；仅一个共享 FFmpeg 生产者，另一个窗口继续播放。
- 前端 TypeScript 检查与 Vite 发布构建通过。

测试入口（先准备依赖及前端产物；只在隔离测试环境执行）：

~~~bash
cd web_console/frontend
npm run build
cd ../..
PYTHONPATH=web_console/backend python3 -m pytest -q \
  web_console/backend/tests/test_stream.py \
  web_console/backend/tests/test_live_stream.py \
  web_console/backend/tests/test_shared_stream_playback.py \
  web_console/backend/tests/test_live_latency.py
python3 -m pytest -q \
  web_console/frontend/tests/test_live_playback_buffer.py \
  web_console/frontend/tests/test_live_stream_recovery.py \
  web_console/frontend/tests/test_live_view_playback.py
~~~

可设置 RK3588_TEST_FRONTEND_DIST 选择隔离构建的 dist；时间码测试另支持 RK3588_TEST_STREAM_ROUTER 选择 A/B 路由源文件，RK3588_TEST_MAX_LIVE_LATENCY_MS 默认为 1200。生产环境不暴露测试源、暂停和时间码 API。

原始测试、基线源码、发布目录备份和 JSON 测量记录位于本机 build/deploy-backups/web-live-latency-20261026-221410/，不随文档发布。尝试读取现场 8554 时应用已停止、连接被拒绝，所以该次采样没有用作性能证据，也没有主动重启视觉应用。

## 本机部署确认

已更新 `/opt/ai_apps/_console` 的前端发布产物及后端 `routers/stream.py`，重启 `rk3588-console.service` 后健康检查返回 200，实际 HTTP 交付的 JS 与候选构建逐字节一致。引擎及应用配置未部署改动，停止的视觉应用保持停止。

Web 重启会清除内存登录会话。重新登录、启动所需视觉应用，并用 Ctrl+F5 重新加载页面；已有标签页中运行的旧 JS 不会因服务器文件更新自动切换播放策略。旧发布目录与后端源码备份均已保留。
