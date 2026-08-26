# 验收清单

## 目录

- [所有改动](#所有改动)
- [Logic 与清单](#logic-与清单)
- [配置和热重载](#配置和热重载)
- [事件与投递](#事件与投递)
- [Web](#web)
- [板端验收](#板端验收)
- [交付声明](#交付声明)

## 所有改动

- [ ] 通过 `develop_feature` 实施时，全部变更只位于 `vision_analysis/src/logic/modules/**` 或
      `vision_analysis/src/logic/global_modules/**`；出现任何其他路径时整批拒绝回写。
- [ ] 白名单内没有符号链接、子模块或特殊文件。
- [ ] `git status --short` 中没有意外文件；没有覆盖用户已有改动。
- [ ] 新名称可在当前源码、配置或需求中找到依据。
- [ ] 没有引用 `src/analyzer`、`src/core`、`src/player` 等已删除目录。
- [ ] 没有引用当前未注册的 Logic。
- [ ] 生成物没有被当作源码手改。
- [ ] 非隔离向导任务的公共行为变化已同步对应 Skill/reference；隔离向导只报告后续文档任务，不越界编辑。

## Logic 与清单

- [ ] 通道模块位于 `src/logic/modules/<name>/`；全局模块位于 `global_modules/<name>/`。
- [ ] 模块至少包含 C++ 源和 `logic.json`。
- [ ] `REGISTER_LOGIC`/`REGISTER_GLOBAL_LOGIC` 的函数名就是外部 ID。
- [ ] 源 `logic.json` 不写 `name`。
- [ ] `parameters` 是 `type=object`、`additionalProperties=false`，每个属性有类型匹配的默认值。
- [ ] 每个 `param_*()` 字面量 key 与 Schema 类型一致。
- [ ] `actions` 与对应 Action 注册宏同时存在或同时不存在。
- [ ] `outputs` 与每个 `publish_*()` 的 key/type 对齐。
- [ ] `event_types` 始终存在；使用 `report_event()` 时不为空且 ID 与 C++ 一致。
- [ ] `report_fields` 与 `event_field()`/`event_json_field()` 对齐。

权威静态检查：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

## 配置和热重载

- [ ] 配置根包含 `global` 对象和 `channels` 数组。
- [ ] `stream.src_type` 明确为 `rtsp`、`file` 或 `usb`。
- [ ] 模型只在 `channels[].models[]`，同通道模型 ID 唯一。
- [ ] ROI 只在 `channels[].roi_zones[]`，坐标为 0–1。
- [ ] 模块参数只在对应实例的 `logic_parameters`。
- [ ] 参数热重载策略与状态语义一致。
- [ ] 没有把通道拓扑、显示尺寸/布局或 RTSP 输出设置误当成可热更新字段。
- [ ] 有可执行文件时对实际配置运行 `./vision_analysis --validate-config ./assets/config_6.json`（示例文件名按应用替换）。

## 事件与投递

- [ ] Logic 不执行 HTTP/Dify、不读取凭据、不编码 Base64。
- [ ] `EventRequest` 每次调用独立构造；合并模式有明确依据。
- [ ] 全局视频 delivery 配置有效 `media_source_channel_id`。
- [ ] delivery 保存 `connection_id`、`contract_id`、`contract_revision` 和 `media`。
- [ ] 模块模板已在模块 `logic.json.report_templates` 声明。
- [ ] 模板 `owner_logic`、`event_types`、字段、媒体和 Adapter 能力对齐。

模板聚合检查会写临时输出：

```bash
cd vision_analysis
tmp_dir="$(mktemp -d)"
python3 scripts/generate_report_templates.py \
  --logic-root src/logic \
  --app-dir report_templates \
  --adapter-catalog ../service/upload/adapters/catalog.json \
  --output "$tmp_dir/report_templates"
rm -rf -- "$tmp_dir"
```

投递服务有改动时运行其现有测试；不要宣称未运行的远端联调成功。

## Web

- [ ] API 调用集中在 `web_console/frontend/src/api/client.ts`。
- [ ] 新 HTTP 路由在 `backend/main.py` 注册；鉴权例外是明确设计而非遗漏。
- [ ] WebSocket 自行校验查询参数中的 token。
- [ ] 配置编辑同时维护 `configToGraph.ts` 与 `graphToConfig.ts` 的往返一致性。
- [ ] 前端改动至少执行 `npm run build`；后端改动执行相关 pytest。
- [ ] 浏览器预览场景检查 `enable_rtsp` 与 H264 编码要求。

## 板端验收

按功能选择，不要求用主机模拟硬件结论：

- [ ] 构建/打包来自当前源码，二进制、`logics.json` 和 `report_templates/` 同版。
- [ ] 真实 RTSP/USB/文件源能够启动、断流恢复符合预期。
- [ ] 画面、ROI、Action、热重载和日志观察点符合需求合同。
- [ ] 事件目录的三份 JSON、媒体终态和 delivery 状态符合预期。
- [ ] 远端联调检查请求预览、HTTP 状态/业务成功条件和幂等。
- [ ] GPIO、NPU、RGA、编码器等硬件路径只在 RK3588 上给出最终结论。

## 交付声明

最终说明必须区分：

- 已静态验证；
- 已构建；
- 已运行单元/集成测试；
- 已在 RK3588 真实设备验证；
- 未验证或需要部署者提供的外部条件。

不要把“代码看起来正确”“事件已接受”或“HTTP 请求已入队”写成端到端成功。
