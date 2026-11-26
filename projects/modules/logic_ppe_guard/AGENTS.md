# RK3588 算法开发约定

- 将 `/userdata/rk3588_visual_analysis_framework/engine/` 视为只读。可以查阅接口和实现，禁止在其中新增、修改、移动或删除文件。
- 开发业务算法时，将新增内容放在 `projects/` 下对应的算法模块。穿戴检测的代码、测试、配置样例、说明文档和辅助脚本都放在 `projects/modules/logic_ppe_guard/` 内。
- 仓库根目录用于框架工程。业务文档、Python 脚本和临时产物不要放在根目录。
- 交付前用 `git status --short` 核对本次变更的位置，保留其他人已有的文件和未提交改动。
