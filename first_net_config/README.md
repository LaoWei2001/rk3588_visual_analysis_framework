# RK3588 现场网络配置工具

统一的交互式网络工具，面向 NetworkManager。程序不会把 `eth0`、`eth1`、
`wlan0` 固定绑定为某种角色；每次操作都会列出网卡、MAC、Link、IPv4、默认路由
和当前 SSH 路径，由用户明确选择。

## 构建与运行

```bash
cd first_net_config
./build.sh
sudo ./first_net_config
```

## 第一版功能

- 显示全部网卡及活动 IPv4 网段，检测不同接口间的相同或包含式网段重叠。
- 在静态地址应用前阻止重复 IP；重叠网段必须输入 `OVERLAP` 才能继续。
- 通过所选有线网口搜索海康 SADP 和 ONVIF 摄像头；读取固件广播的 IP、掩码、
  网关、MAC、型号、序列号及端口。固件未广播的字段允许人工填写。
- 只配置 RK3588 摄像头侧地址，不修改摄像头内部配置，不需要摄像头账号密码。
- 配置任意所选有线网口、普通 PSK/SAE/开放 Wi-Fi，以及笔记本直连维护网络。
- 每次新配置必须明确选择“临时”或“永久”，并再次输入 `TEMPORARY` 或
  `PERMANENT`。临时连接使用 NetworkManager `save no`，不会写入磁盘。
- 通过临时 profile、独立 watchdog 和跨 SSH 会话确认保护网络切换；未确认时
  自动恢复。
- 活动连接不能从删除菜单直接删除。
- 提供独立的“清除所有 NetworkManager 连接配置”高风险功能。可以只删除磁盘上的
  永久连接、等待重启后生效，也可以立即删除全部临时、永久及活动连接；执行前会
  显示清单、保存紧急备份并要求输入完整确认短语。立即清除会把当前 SSH 连接放在
  最后处理，但 SSH 最终仍会断开。
- 保存设备初始 NetworkManager/摄像头应用配置基准，预览后永久恢复；恢复前自动
  保存紧急备份，写回配置后由用户在具备本地维护手段时重启生效。

## 摄像头发现的边界

SADP/ONVIF 发现通常无需认证，但结果由摄像头固件决定。IP 一般可以获得；掩码、
网关和服务端口可能不广播。工具不会修改摄像头 IP、激活摄像头或处理摄像头密码。

## Wi-Fi 边界

支持开放网络、普通共享密码 WPA/WPA2-PSK 和 WPA3-SAE。不支持需要用户名、身份
证书或 802.1X 的企业 Wi-Fi。

## 通过 SSH 修改当前网卡

切换当前 SSH 使用的网卡后，原会话可能立即断开。这不表示配置失败。

1. 记录程序显示的新 IP。
2. 使用新 IP 重新建立 SSH。
3. 在 120 秒内再次运行同一个程序。
4. 输入大写 `YES` 确认；输入 `ROLLBACK` 立即恢复。

事务保存在 `/run/first_net_config`，权限为 root 专用。待确认期间不要重启设备。

## 系统初始基准

初始基准只能在制作镜像或确认设备仍处于初始状态时保存。工具不会自动把已经修改
过的当前配置当成出厂配置。基准默认保存在：

```text
/var/lib/first_net_config/
```

基准绑定 `/etc/machine-id` 并带 SHA-256 校验，包含 NetworkManager 系统连接文件和
Web Console 摄像头持久化配置。恢复会先保存 root-only 紧急备份，不会自动重启。

## 清除所有连接配置

菜单 16 不依赖系统初始基准。它清除的是 NetworkManager 的连接 profile，包括
有线静态 IP、DHCP、Wi-Fi 名称和密码等，不会删除 NetworkManager 程序、网卡驱动、
系统级 NetworkManager 策略、初始基准或 Web Console 的摄像头发现信息。

- “重启后生效”只清除 `/etc/NetworkManager/system-connections` 中的永久 profile，
  不主动 reload 或断开当前连接；临时 profile 会随重启消失。NetworkManager 仍可能
  自行检测文件变化，所以通过 SSH 操作时也必须做好断线准备。
- “立即清除”通过 UUID 删除 NetworkManager 当前已知的全部 profile，并清理残留的
  永久和运行时 keyfile。它会造成 Wi-Fi、网线和 SSH 连接中断。
- 紧急备份保存在 `/var/lib/first_net_config/backups/`。NetworkManager 可能根据系统
  策略生成新的默认有线 profile；这种 profile 不是清除前的旧配置。

## 当前限制

- 第一版恢复采用“写回基准、重启后生效”，不会自动重启设备。
- 摄像头配置使用完整摄像头子网；检测到与其他网卡重叠时会严重警告，但尚未自动
  改用 `/32` 精确隔离路由。
- 还没有覆盖 VLAN、PPPoE、802.1X、蜂窝网络和 IPv6 配置。
