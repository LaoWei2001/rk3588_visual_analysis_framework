# 旧辅助脚本边界

本 Skill 的 `scripts/` 中三个 Python 脚本均创建于 2026-08-14，本轮遵守“不修改代码”约束，原文件
保持不变。它们是辅助启发式工具，不是当前框架契约或验收真源。

| 脚本 | 当前可用范围 | 已知缺口 |
|---|---|---|
| `audit_docs.py` | 补充检查 Markdown 链接、部分源码路径和少量过期语句 | 规则表不是完整事实清单，不能替代源码反查、Skill 校验或项目测试 |
| `scaffold_logic.py` | 生成最小通道/全局模块骨架 | 不收集业务合同，也不生成 outputs、Action、上报模板或完整验收；生成后必须人工补齐 |
| `validate_logic.py` | 对部分注册、manifest、字段和阻塞调用做启发式检查 | 仍把全局 Action 判为“不支持”，与当前 `REGISTER_GLOBAL_LOGIC_ACTION` 实现冲突 |

当前权威入口：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

再按目标 Skill 的验收段落运行配置、Web、上传、构建和板端测试。不得因这些旧脚本返回成功就声明
功能与当前引擎完全一致；`validate_logic.py` 对含全局 Action 的模块报错也不能作为移除功能的依据。
