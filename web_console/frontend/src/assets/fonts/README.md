# 添加终端字体

每个一级文件夹代表一个字体，Web“终端字体”下拉框直接显示该文件夹的名字。
字体文件本身的名字可以自定，不需要修改 TypeScript、CSS 或字体清单。

```text
fonts/
├── Departure Mono/
│   ├── DepartureMono-Regular.woff2
│   ├── OFL.txt
│   └── README.md
├── VT323/
│   ├── VT323-Regular.ttf
│   └── OFL.txt
└── 我的新字体/
    ├── 任意文件名.ttf
    └── LICENSE.txt
```

- 支持 `.woff2`、`.woff`、`.ttf`、`.otf`，也支持大写扩展名和字体目录内的子目录。
- 一个字体文件夹放同一套字体；存在多个文件时优先 Regular/Normal/Book/Roman 常规字形，
  再按 WOFF2 → WOFF → TTF → OTF 选择。其他格式作为同一字形的加载后备，不生成重复选项。
  未提供独立字重的粗体/斜体继续由浏览器合成。
- `OFL`、`LICENSE`、`LICENCE`、`COPYING` 命名的 TXT/MD 或无扩展名文件会自动作为字体许可链接。
  请随字体保留其作者声明与许可。
- 字体文件仅在选择时加载；空文件夹、只有说明文件的文件夹不会生成选项。
- 字体选项保存在浏览器，以文件夹名作为标识。改名/删除已选字体后会回退到默认字体。
- 自带字体“Departure Mono”为默认；保留“现代等宽”系统字体选项。

## 生效

开发模式由 Vite 自动发现新增文件；生产 Web 使用构建产物，放好目录后执行：

```bash
cd /userdata/rk3588_visual_analysis_framework/web_console/frontend
npm run build
```

再将 `dist/` 部署到运行中的控制台静态目录（本机为 `/opt/ai_apps/_console/frontend/dist/`），并刷新浏览器。
只添加源码目录文件不会自动改变已经部署的生产页面。
