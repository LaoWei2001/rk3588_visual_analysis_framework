# first_net_config

面向 NetworkManager 的交互式网络配置工具。构建：

```bash
cd first_net_config
./build.sh
sudo ./first_net_config
```

## 通过 SSH 修改当前网卡的 IP

工具切换当前 SSH 正在使用的网卡时，原 SSH 连接可能立即断开。这不表示配置失败。

1. 在原会话中填写新配置并开始测试。
2. 使用新 IP 重新建立 SSH 连接。
3. 在 120 秒确认期限内再次运行同一个 `first_net_config`。
4. 工具会在显示菜单前识别待确认配置。输入大写 `YES` 后，新配置会正式保存并设置为开机自动连接。

输入 `ROLLBACK` 会立即恢复旧网络；退出或输入其他内容不会提交，超时后自动恢复。

待确认事务保存在 `/run/first_net_config`，权限为 root 专用，其中不包含 Wi-Fi 密码。回退监控优先由 systemd 临时服务承载；没有可用 systemd 时使用脱离终端的守护进程。待确认期间不要重启设备。
