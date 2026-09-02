# 大模型任务提示词模板

这些模板用于约束执行范围，不替代目标源码。将尖括号内容替换为真实需求；能从仓库确定的内容由模型自动核对，不要求用户重复提供。

## 目录

- [交互式总入口](#交互式总入口)
- [通用前缀](#通用前缀)
- [新增通道 Logic](#新增通道-logic)
- [新增全局 Logic](#新增全局-logic)
- [新增或修改上报](#新增或修改上报)
- [Web 界面改动](#web-界面改动)
- [引擎公共能力改动](#引擎公共能力改动)

## 交互式总入口

不想手工填写模板时，在仓库根目录运行：

```bash
./develop_feature
```

原生 Windows 使用：

```powershell
develop_feature.cmd
```

也可以直接附带一句初始描述：

```bash
./develop_feature "新增人员越界报警，并通过 HTTP 上报带标注图片"
```

向导正式支持 Codex CLI 和 Claude Code。只安装一个时自动使用，两个都可用时启动菜单让用户选择；也可
明确指定：

```bash
./develop_feature --agent codex "新增人员越界报警"
./develop_feature --agent claude "新增人员越界报警"
```

选定代理后，向导检测当前开发宿主，第一题只让用户用“是”或“否 + 简短纠正”确认检测结果和默认
RK3588 部署目标。业务需求通常只问 2–3 轮、最多 4 轮：向导先核对源码，再给出合并后的行为与交付
方案供用户简短确认，不逐项盘问内部合同字段。满足需求完整性门后默认自动开发。使用
`./develop_feature --confirm-before-code` 可在写代码前审阅合同；使用 `--plan-only` 只输出合同和计划，
使用 `--check` 检查两个代理的可执行文件、版本参数和全部 Skill。

自动开发在一次性隔离副本中进行，不要求用户切换 `/permissions`。原仓库只接受
`vision_analysis/src/logic/modules/**` 和 `vision_analysis/src/logic/global_modules/**` 的文件变化；任何越界
改动都会让整批结果被拒绝。需要 Web、服务、配置解析或公共引擎修改的需求不由此入口实施。
直接复制本页后续提示词给模型不具备机械写回保护；要求文件保证时必须使用 `develop_feature`。

## 通用前缀

```text
在当前 rk3588_visual_analysis_framework 仓库中完成以下任务。

硬约束：
1. 先读取匹配的 docs/skills/*/SKILL.md 及其要求的参考页。
2. 用 rg 查当前源码、REGISTER_* 宏、logic.json、Web 路由和调用方；文档冲突时以源码为准并同步修文档。
3. 保留工作区已有改动，不修改无关文件，不手改生成物。
4. 不使用已删除路径或当前未注册的 Logic。
5. 完成后运行与风险相称的现有校验，并准确报告未验证项。
```

## 新增通道 Logic

```text
使用 $rk3588-channel-logic 新增 <logic_name>。

输入：<模型/标签/ROI/是否允许无推理>
触发：<当帧条件、持续时间、多目标规则>
复位与去重：<规则>
参数：<key、类型、默认值、范围、热重载策略>
输出：<绘制、publish outputs、Action>
上报：<event type、fields、merge mode、媒体；不需要则明确写无>

只在 metadata 驱动不足时修改框架或 Web。先给出需求合同和预计文件，再实施、校验并说明状态生命周期。
```

## 新增全局 Logic

```text
使用 $rk3588-global-logic 新增 <global_logic_name>。

输入通道来源：<画布连接/空连接时应用全部通道>
上游 outputs 契约：<key:type>
聚合条件：<规则>
输入不足或过期：<行为>
状态、闩锁和复位：<规则>
Action：<id/行为；不需要则无>
上报：<事件字段、来源通道选择、图片、视频来源>

使用 gctx->inputs() 处理普通业务，只有确需版本/媒体一致性时才用原始快照接口。
```

## 新增或修改上报

```text
使用 $rk3588-console-ops 完成 <Logic> 的 <远端系统> 上报。

远端协议：<method/path/encoding/headers/字段/文件/成功条件>
连接信息：<HTTP 或 Dify；密钥只放应用连接配置>
事件：<event type 与 fields>
媒体：<annotated_image/raw_image/video/纯数据>
兼容要求：<复用或修改包模板/创建新 ID 自定义模板、是否保留旧 revision；包模板同 ID 时优先>

先判断只需改连接、契约、Logic 事件字段还是 Adapter。禁止在 Logic 中直接联网，也禁止把业务字段硬编码进通用 Adapter。
```

## Web 界面改动

```text
使用 $rk3588-console-ops 修改 Web：<功能>。

用户入口：<页面/按钮/流程>
后端能力：<现有或新增 API>
鉴权：<默认登录保护；若要求免登录必须明确说明>
状态与失败提示：<要求>
配置往返：<是否影响 configToGraph/graphToConfig>

先检查当前路由、api/client.ts 和 React 调用方。完成后运行后端相关测试与 frontend npm run build。
```

## 引擎公共能力改动

```text
使用 $rk3588-src-modules 修改 <模块/API/配置>。

现有瓶颈：<可复现证据>
新契约：<调用方、数据所有权、线程、生命周期>
兼容范围：<配置/Logic/Web/媒体/服务>
性能与硬件约束：<要求>

先画出当前调用链和所有调用方，再提出最小改动。同步更新受影响的 Skill，不能只改头文件或单一路径。
```
