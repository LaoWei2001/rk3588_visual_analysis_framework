# logos —— 随机 logo / GIF 目录

把**图片或 GIF** 放进这个文件夹即可。用户**每次打开网页**（侧边栏 logo 与登录页 logo），后端会从这里**随机**取一张显示；GIF 会自动播放（动图）。

## 这个文件夹在哪

- **开发机（源码）**：`web_console/frontend/logos/`
- **板子（安装后）**：`/opt/ai_apps/_console/frontend/logos/` ← 在板子上往这里放图
  > `install.sh` 会把本目录拷过去；重装用 `cp -n` 合并，**不会覆盖**你在板子上已放的图。
  > 若你是旧版本装的、板子上没有这个目录，重新跑一遍 `install.sh` 即可创建（或手动 `mkdir -p /opt/ai_apps/_console/frontend/logos`）。
- 加/删图**即时生效**，无需重启服务、无需重新构建（后端每次请求实时扫描本目录）。

- 支持的格式：`.png .jpg .jpeg .gif .webp .bmp .svg .apng`
- 想加图就直接丢进来、想删就删掉文件，**不用重新编译、不用改代码**（后端每次请求实时扫描本目录）。
- 这个文件夹**为空或不存在**时，回退使用上一级的 `logo.png`。
- 建议用接近正方形、尺寸别太大的图（侧边栏头像不大）。

## 原理

- 后端接口：`GET /logo/random` —— 每次请求随机返回本目录里的一张（`web_console/backend/main.py`）。
- 前端：侧边栏 `SidebarLogo`（`src/App.tsx`）和登录页（`src/pages/LoginPage.tsx`）的 `<img>` 指向 `/logo/random?t=<时间戳>`；时间戳让每次打开都重新向后端请求，从而重新随机。

> 注：这是后端直接读取的源目录，**不经过前端构建**，所以板子上换图无需 `npm run build`。
