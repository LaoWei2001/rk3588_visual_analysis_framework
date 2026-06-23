import asyncio
import websockets
import json
import hashlib
import shutil
import os
import requests
import threading
import uuid

# ================= 配置区 =================
# 资产目录: 优先用环境变量 ASSETS_DIR, 未设置时自动检测
#   源目录调试: ASSETS_DIR 未设置 → 自动找到 ../../rk3588_yolo/assets
#   dist 部署:   ASSETS_DIR 由启动脚本设置为 dist/assets
def _resolve_assets_dir():
    env_dir = os.environ.get("ASSETS_DIR", "")
    if env_dir:
        return os.path.abspath(env_dir)
    # 自动检测: 从 service/model_update/ 向上到项目根, 再找 rk3588_yolo/assets
    script_dir = os.path.dirname(os.path.abspath(__file__))
    # 源目录: service/model_update/../../rk3588_yolo/assets
    candidate = os.path.normpath(os.path.join(script_dir, "..", "..", "rk3588_yolo", "assets"))
    if os.path.isdir(candidate):
        return candidate
    # dist 部署降级: dist/services/model_update/../../assets = dist/assets
    candidate = os.path.normpath(os.path.join(script_dir, "..", "..", "assets"))
    if os.path.isdir(candidate):
        return candidate
    raise FileNotFoundError("无法定位 assets 目录, 请设置环境变量 ASSETS_DIR")

ASSETS_DIR = _resolve_assets_dir()


def _load_ota_config():
    """读取本目录 ota_config.json（由网页控制台写入）；缺失/损坏则用默认值。
    用标准库 json，不给本服务新增依赖。"""
    cfg_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ota_config.json")
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


_OTA_CFG = _load_ota_config()

# 目标配置文件：默认 config.json —— 必须与控制台/ C++ 实际运行的那份一致。
# （历史上写死 config_mux16_v5.json，导致 OTA 换的模型热重载不进正在跑的进程，此处修正）
# 优先级：环境变量 CONFIG_FILE > ota_config.json > 默认 config.json
_TARGET_CONFIG = os.environ.get("CONFIG_FILE") or _OTA_CFG.get("target_config") or "config.json"
CONFIG_PATH = os.path.join(ASSETS_DIR, _TARGET_CONFIG)

# 平台 WebSocket 基础地址。优先级：环境变量 PLATFORM_WS_HOST > ota_config.json > 默认
PLATFORM_WS_HOST = (os.environ.get("PLATFORM_WS_HOST")
                    or _OTA_CFG.get("platform_ws_host")
                    or "tunnel.memanager.cn")
# ====================================================

def get_device_id():
    """获取边缘端底层物理 MAC 地址并按照平台规则转换为 DeviceID"""
    raw_mac = uuid.UUID(int=uuid.getnode()).hex[-12:].upper()
    mac_display = ":".join([raw_mac[i:i+2] for i in range(0, 12, 2)])
    print(f"[Sys] 成功获取本地物理 MAC 地址: {mac_display}")
    device_id = hashlib.md5(raw_mac.encode('utf-8')).hexdigest().upper()
    return device_id

def get_local_version(channel_id):
    """从本地读取指定通道的模型版本号（通过匹配 id）"""
    try:
        with open(CONFIG_PATH, 'r') as f:
            config = json.load(f)
            # 遍历 channels 列表，找到对应 id 的对象
            for ch in config.get('channels', []):
                if ch.get('id') == channel_id:
                    return str(ch.get('version', ''))
            return "" # 如果没有找到对应通道，返回空
    except Exception:
        return ""

def execute_update(data, ws_ip_port, ws_loop, websocket):
    """
    数据流管道：在独立线程中异步执行 HTTP 下载、校验、替换与热重载生效。
    """
    version = data.get("version", "")
    model_type = data.get("type", "yolov5")
    
    try:
        url_path = data.get('url')
        target_md5 = data.get('md5')
        if not target_md5:
            print("[OTA] 严重异常：缺少 md5 字段，拒绝更新")
            send_feedback_safe(ws_loop, websocket, -1, version)
            return
        if not isinstance(target_md5, str):
            print(f"[OTA] 严重异常：md5 字段类型错误: {type(target_md5)}")
            send_feedback_safe(ws_loop, websocket, -1, version)
            return
        
        # 解析通道号
        raw_channel = data.get('channel')
        if raw_channel in [None, ""]:
            channel_id = 0
        else:
            channel_id = int(raw_channel)
        
        # 1. 拼接 HTTPS 下载完整地址
        download_url = f"https://{ws_ip_port}{url_path}"
        print(f"[OTA] 启动数据通道，正在从 {download_url} 拉取模型...")
        
        # 2. 下载至虚拟磁盘
        temp_file = f"/tmp/model_update_ch{channel_id}.rknn"
        
        with requests.get(download_url, stream=True, timeout=60) as r:
            r.raise_for_status()
            with open(temp_file, 'wb') as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)
            
        # 3. MD5 指纹哈希校验
        hash_md5 = hashlib.md5()
        with open(temp_file, "rb") as f:
            for chunk in iter(lambda: f.read(4096), b""):
                hash_md5.update(chunk)
        local_md5 = hash_md5.hexdigest().lower()

        if local_md5 != target_md5.lower():
            print(f"[OTA] 严重异常：MD5 校验失败！")
            os.remove(temp_file)
            send_feedback_safe(ws_loop, websocket, -1, version)
            return

        # 4. 准备替换文件，读取配置文件
        with open(CONFIG_PATH, 'r') as f:
            config = json.load(f)

        # ====== 核心修改：通过 id 查找目标通道配置 ======
        target_channel = None
        for ch in config.get('channels', []):
            if ch.get('id') == channel_id:
                target_channel = ch
                break
        
        if target_channel is None:
            print(f"[OTA] 严重异常：配置文件中未找到 id 为 {channel_id} 的通道！")
            os.remove(temp_file)
            send_feedback_safe(ws_loop, websocket, -1, version)
            return
        # ===============================================

        old_model_path = target_channel.get('model_path', '')
        if old_model_path:
            # 旧模型路径相对于 assets 的父目录 (即项目根或 dist 根)
            project_root = os.path.dirname(ASSETS_DIR)
            old_full_path = os.path.join(project_root, old_model_path.lstrip('./'))
            if os.path.exists(old_full_path):
                print(f"[OTA] 发现旧模型: {old_full_path}，已按要求保留不删除。")

        new_file_name = f"model_ch{channel_id}_{local_md5[:8]}.rknn"
        target_path = os.path.join(ASSETS_DIR, new_file_name)
        shutil.move(temp_file, target_path)

        # 5. 更新 config.json 配置文件中的目标通道字典
        # 保持与原有配置的相对路径格式一致 "assets/..."
        target_channel['model_path'] = f"assets/{new_file_name}"
        target_channel['version'] = str(version)
        target_channel['model_type'] = model_type
        
        # 写回配置文件
        with open(CONFIG_PATH, 'w') as f:
            json.dump(config, f, indent=4)
            
        print(f"[OTA] 底层替换完成。模型路径: {new_file_name}, 类型: {model_type}")

        # 6. 主动上报成功状态
        send_feedback_safe(ws_loop, websocket, 1, version)

        # 7. 当前程序支持模型热重载，无需重启进程
        print("[OTA] 配置已更新，等待业务进程自动热重载新模型。")

    except Exception as e:
        print(f"[OTA] 流程崩溃退出: {e}")
        send_feedback_safe(ws_loop, websocket, -1, version)

def send_feedback_safe(loop, websocket, status, version):
    """线程安全的 WebSocket 消息发送包裹函数"""
    payload = {
        "action": "UPDATE_COMMAND_PROGRESS",
        "status": status,
        "version": version
    }
    asyncio.run_coroutine_threadsafe(websocket.send(json.dumps(payload)), loop)

async def ota_daemon():
    """边缘端主守护进程"""
    device_id = get_device_id()
    print(f"[Sys] 配置文件路径: {CONFIG_PATH}")
    ws_url = f"wss://{PLATFORM_WS_HOST}/ws/device/{device_id}"
    ws_ip_port = PLATFORM_WS_HOST
    
    while True:
        try:
            print(f"[Sys] 尝试接入平台: {ws_url}")
            async with websockets.connect(ws_url, ping_interval=30, ping_timeout=10) as websocket:
                print(f"[Sys] 成功连接。DeviceID: {device_id}")
                
                while True:
                    message = await websocket.recv()
                    print(f"\n[Sys] 收到平台原始消息: {message}")
                    
                    try:
                        data = json.loads(message)
                    except json.JSONDecodeError:
                        print("[Sys] 警告: 非标准 JSON 格式，已忽略。")
                        continue
                    
                    if data.get("action") == "UPDATE_COMMAND":
                        target_version = data.get("version")
                        
                        # 解析通道号并转换为 int
                        raw_channel = data.get('channel')
                        ch_id = int(raw_channel) if raw_channel not in [None, ""] else 0

                        local_version = get_local_version(ch_id)
                        print(f"[Sys] 指令解析 -> 通道: {ch_id}, 目标版本: {target_version}, 本地版本: {local_version}")

                        if str(target_version) == str(local_version):
                            print(f"[Sys] 版本一致，跳过下载。")
                            feedback = {
                                "action": "UPDATE_COMMAND_PROGRESS",
                                "status": 1,
                                "version": target_version
                            }
                            await websocket.send(json.dumps(feedback))
                            continue
                            
                        feedback = {
                            "action": "UPDATE_COMMAND_PROGRESS",
                            "status": 0,
                            "version": target_version
                        }
                        await websocket.send(json.dumps(feedback))
                        
                        loop = asyncio.get_running_loop()
                        threading.Thread(target=execute_update, args=(data, ws_ip_port, loop, websocket), daemon=True).start()
                        
        except Exception as e:
            print(f"[Sys] 平台通讯链路中断: {e}。5秒后自动重连...")
            await asyncio.sleep(5)

if __name__ == "__main__":
    asyncio.run(ota_daemon())
