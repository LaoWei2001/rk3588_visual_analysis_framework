# 开发辅助脚本

脚手架与校验脚本已支持外部项目：

```bash
python3 docs/skills/build-rk3588-vision-app/scripts/scaffold_logic.py --project /path/to/app --kind channel --name logic_demo --label Demo
python3 docs/skills/build-rk3588-vision-app/scripts/validate_logic.py logic_demo --project /path/to/app
```

模块 schema 的权威校验仍是引擎生成器；脚本中的阻塞调用提示用于辅助审查，不替代业务测试。
全局 Action 已按 `REGISTER_GLOBAL_LOGIC_ACTION` 验证。新项目优先使用 `vision new-logic` 和 `vision check`。
