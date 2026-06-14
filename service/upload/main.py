#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Unified upload consumer (Multi-threaded version).

Two worker threads run in parallel:
- Thread 1: OutboxForwarder -> 扫描本地发件箱目录(C++落盘), 补传告警到业务服务器, 传成功即删本地
- Thread 2: dify_queue (Redis) -> upload image and trigger Dify workflow
"""

import base64
import io
import json
import os
import signal
import sys
import time
import threading
from typing import Any, Dict

import redis
import requests
import yaml

# 全局关闭标志, 由 SIGTERM/SIGINT handler 设置
_shutdown_event = threading.Event()


class OutboxForwarder:
    """扫描本地"发件箱"目录(C++ 报警时落盘), 把每条告警补传到业务服务器; 传成功即删本地。

    行为(对应"送达即删 / 断网攒着 / 通了边传边删"):
    - 平台/网络不可达(requests 抛异常)→ 整轮退避 RETRY_WAIT 秒再重扫, 不疯狂重试。
    - 服务器拒绝(非 200)→ 跳过该条、不删除, 下一轮再试, 不阻塞其它记录。
    - 传成功(200)→ 立刻删除该条的 .json + 图片。
    - 空闲(无待传)→ 每 SCAN_IDLE 秒轮询一次。
    地址方案2: 优先用记录自带的 server_url, 留空回落到 config.yaml 默认。
    """

    SCAN_IDLE = 3.0    # 无待传时的轮询间隔(秒)
    RETRY_WAIT = 15.0  # 平台不可达时的退避(秒)

    def __init__(self, config: Dict[str, Any], store_dir: str):
        self.default_url = config["server"]["url"]
        self.timeout = config["server"]["timeout"]
        self.store_dir = store_dir

    def _list_pending(self):
        """返回待传记录的 .json 文件名, 按 mtime 从旧到新(先补最早攒下的)。"""
        try:
            names = [f for f in os.listdir(self.store_dir) if f.endswith(".json")]
        except FileNotFoundError:
            return []
        def _mtime(n):
            p = os.path.join(self.store_dir, n)
            try:
                return os.path.getmtime(p)
            except OSError:
                return 0.0
        names.sort(key=_mtime)
        return names

    def _safe_unlink(self, path: str):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        except Exception as e:
            print(f"[Outbox] remove failed {path}: {e}")

    def _delete_record(self, meta: Dict[str, Any], jname: str):
        # 先删 .json(它一旦消失, 这条就不会被再次列出/补传), 再删图片
        self._safe_unlink(os.path.join(self.store_dir, jname))
        for k in ("img", "img_raw"):
            n = meta.get(k, "")
            if n:
                self._safe_unlink(os.path.join(self.store_dir, n))

    def _img_b64(self, fname: str) -> str:
        if not fname:
            return ""
        p = os.path.join(self.store_dir, fname)
        if not os.path.exists(p):
            return ""
        with open(p, "rb") as f:
            return base64.b64encode(f.read()).decode()

    def _forward_one(self, jname: str) -> str:
        """补传一条; 返回 'sent' | 'skip' | 'rejected' | 'down'。"""
        jpath = os.path.join(self.store_dir, jname)
        try:
            with open(jpath, "r", encoding="utf-8") as f:
                meta = json.load(f)
        except FileNotFoundError:
            return "skip"   # 已被删除/淘汰
        except Exception as e:
            print(f"[Outbox] 损坏的元数据 {jname}: {e}")
            return "skip"   # 跳过, 不阻塞其它记录

        try:
            img_b64 = self._img_b64(meta.get("img", ""))
            raw_b64 = self._img_b64(meta.get("img_raw", ""))
        except Exception as e:
            print(f"[Outbox] 读图失败 {jname}: {e}")
            return "skip"

        url = meta.get("server_url") or self.default_url
        payload = {
            "source": "JNU",
            "eventType": "4005",
            "detResult": {},
            "snapTime": meta.get("snapTime", ""),
            "endTime": meta.get("endTime", ""),
            "base64Data": img_b64,
            "base64DataRaw": raw_b64 or img_b64,
            "invadeFlag": 1,
        }
        try:
            resp = requests.post(url, json=payload, timeout=self.timeout)
        except Exception as e:
            print(f"[Outbox] 平台不可达, 稍后重试 ch={meta.get('camera_id')}: {e}")
            return "down"

        if resp.status_code == 200:
            self._delete_record(meta, jname)
            print(f"[Outbox] 已补传并删除 ch={meta.get('camera_id')} {jname} url={url}")
            return "sent"

        print(f"[Outbox] 服务器拒绝 status={resp.status_code} ch={meta.get('camera_id')} (下一轮再试)")
        return "rejected"

    def run(self):
        print(f"[Outbox] forwarder started, store={self.store_dir}")
        while not _shutdown_event.is_set():
            pending = self._list_pending()
            if not pending:
                _shutdown_event.wait(self.SCAN_IDLE)
                continue

            sent = 0
            down = False
            for jname in pending:
                if _shutdown_event.is_set():
                    break
                r = self._forward_one(jname)
                if r == "sent":
                    sent += 1
                elif r == "down":
                    down = True
                    break   # 平台不可达, 本轮到此为止

            if down:
                # 平台不可达: 退避后整轮重来(数据都还在本地, 网页可见)
                _shutdown_event.wait(self.RETRY_WAIT)
            elif sent == 0:
                # 一条都没传成功(都被拒/跳过): 别空转, 歇一会再扫
                _shutdown_event.wait(self.RETRY_WAIT)
            # sent > 0: 立即重扫, 快速排空积压
        print("[Outbox] forwarder exiting (shutdown)")


class DifyUploader:
    def __init__(self, config: Dict[str, Any]):
        # 方案2: config.yaml 的地址/密钥降级为"默认值", 消息自带时优先用消息里的
        self.default_api_url = str(config["dify"]["api_url"])
        self.default_api_key = config["dify"]["api_key"]
        self.timeout = config["dify"]["timeout"]

    @staticmethod
    def _derive_base(api_url: str) -> str:
        """把用户填的 api_url 归一化成 base_url (去掉可能带的 /v1/... 尾巴)."""
        raw_url = str(api_url).rstrip("/")
        if raw_url.endswith("/v1/files/upload"):
            return raw_url[: -len("/v1/files/upload")]
        if raw_url.endswith("/v1/workflows/run"):
            return raw_url[: -len("/v1/workflows/run")]
        return raw_url

    def _upload_file(self, img_b64: str, user: str, upload_url: str, api_key: str) -> str:
        img_bytes = base64.b64decode(img_b64)
        files = {
            "file": ("alarm.jpg", io.BytesIO(img_bytes), "image/jpeg"),
        }
        headers = {"Authorization": f"Bearer {api_key}"}
        response = requests.post(
            upload_url,
            files=files,
            data={"user": user},
            headers=headers,
            timeout=self.timeout,
        )

        if response.status_code != 201:
            print(f"[DifyUploader] file upload failed: status={response.status_code} body={response.text}")
            return ""

        try:
            return str(response.json().get("id", ""))
        except Exception:
            print(f"[DifyUploader] upload response is not JSON: {response.text}")
            return ""

    def _run_workflow(self, payload: Dict[str, Any], workflow_url: str, api_key: str) -> bool:
        headers = {
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        }
        response = requests.post(
            workflow_url,
            json=payload,
            headers=headers,
            timeout=self.timeout,
        )

        if response.status_code != 200:
            print(f"[DifyUploader] workflow failed: status={response.status_code} body={response.text}")
            return False

        try:
            result = response.json()
            data   = result.get("data", {})
            status = data.get("status", "unknown")

            # 工作流本身执行失败（HTTP 200 但业务失败）
            if status == "failed":
                print(f"[DifyUploader] workflow execution failed: error={data.get('error')}")
                return False

            outputs      = data.get("outputs", {})
            elapsed      = data.get("elapsed_time", 0)
            total_tokens = data.get("total_tokens", 0)
            total_steps  = data.get("total_steps", 0)
            run_id       = result.get("workflow_run_id", "")

            print(
                f"[DifyUploader] workflow success: "
                f"run_id={run_id} status={status} "
                f"steps={total_steps} tokens={total_tokens} elapsed={elapsed:.2f}s"
            )
            print(f"[DifyUploader] outputs={outputs}")
            return True

        except Exception as e:
            print(f"[DifyUploader] failed to parse response: {e}, body={response.text}")
            return False

    def upload(self, data: Dict[str, Any]) -> bool:
        img_b64 = data.get("base64Data", "")
        if not img_b64:
            print(f"[DifyUploader] base64Data missing: event_id={data.get('event_id', '?')}")
            return False

        try:
            user = str(data.get("user", "rk3588-test"))
            response_mode = str(data.get("response_mode", "blocking"))
            image_var_name = str(data.get("image_var_name", "image"))

            raw_inputs = data.get("inputs", {})
            if not isinstance(raw_inputs, dict):
                print(f"[DifyUploader] inputs must be dict: {raw_inputs}")
                return False

            workflow_inputs = dict(raw_inputs)
            for key in ("target_id", "channel", "alarm_type", "event_id", "prompt"):
                if key in data and key not in workflow_inputs:
                    workflow_inputs[key] = data[key]

            # 方案2: 地址/密钥随消息走, 留空回落到 config.yaml 默认
            api_url = data.get("dify_api_url") or self.default_api_url
            api_key = data.get("dify_api_key") or self.default_api_key
            base_url = self._derive_base(api_url)
            upload_url = f"{base_url}/v1/files/upload"
            workflow_url = f"{base_url}/v1/workflows/run"

            file_id = self._upload_file(img_b64, user, upload_url, api_key)
            if not file_id:
                return False

            workflow_inputs[image_var_name] = {
                "type": "image",
                "transfer_method": "local_file",
                "upload_file_id": file_id,
            }

            payload = {
                "inputs": workflow_inputs,
                "response_mode": response_mode,
                "user": user,
            }

            ok = self._run_workflow(payload, workflow_url, api_key)
            if ok:
                print(f"[DifyUploader] upload+workflow success: event_id={data.get('event_id', '')} file_id={file_id} api_url={api_url}")
            return ok
        except requests.exceptions.Timeout:
            print("[DifyUploader] request timeout")
            return False
        except Exception as e:
            print(f"[DifyUploader] upload exception: {e}")
            return False


def _connect_redis(config: Dict[str, Any], queue_name: str) -> redis.Redis:
    """建立 Redis 连接, 带指数退避重试 (启动时 Redis 可能未就绪)."""
    max_retries = 12
    base_delay = 1.0
    max_delay = 60.0

    for attempt in range(max_retries + 1):
        try:
            client = redis.Redis(
                host=config["redis"]["host"],
                port=config["redis"]["port"],
                db=config["redis"]["db"],
                decode_responses=True,
            )
            client.ping()
            return client
        except Exception as e:
            delay = min(base_delay * (2 ** attempt), max_delay)
            if attempt < max_retries and not _shutdown_event.is_set():
                print(f"[Worker] Redis unreachable for queue {queue_name} "
                      f"(attempt {attempt+1}/{max_retries+1}), retry in {delay:.0f}s: {e}")
                _shutdown_event.wait(delay)
            else:
                raise

    raise RuntimeError("unreachable")


def queue_worker(config: Dict[str, Any], queue_name: str, uploader_instance: Any):
    """Independent worker thread consuming a single Redis queue."""
    try:
        client = _connect_redis(config, queue_name)
        print(f"[Worker] connected to Redis for queue: {queue_name}")
    except Exception as e:
        print(f"[Worker] failed to connect to Redis for queue {queue_name}: {e}")
        return

    while not _shutdown_event.is_set():
        try:
            # 有限超时, 允许线程定期检查 _shutdown_event
            result = client.blpop(queue_name, timeout=5)
            if not result:
                continue

            _, raw_data = result
            try:
                data = json.loads(raw_data)
            except json.JSONDecodeError as e:
                print(f"[Worker] invalid JSON from queue={queue_name}: {e}")
                continue

            print(f"[Worker] processing message from queue={queue_name}")
            uploader_instance.upload(data)

        except redis.exceptions.ConnectionError:
            if _shutdown_event.is_set():
                break
            print(f"[Worker] Redis connection lost in queue {queue_name}, retrying...")
            time.sleep(2)
        except Exception as e:
            if _shutdown_event.is_set():
                break
            print(f"[Worker] exception in queue {queue_name}: {e}")
            time.sleep(1)

    print(f"[Worker] {queue_name} worker exiting (shutdown)")


def _shutdown_handler(signum, frame):
    """SIGTERM/SIGINT 优雅关闭入口."""
    print(f"[Sys] Received signal {signum}, initiating graceful shutdown...")
    _shutdown_event.set()


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "config.yaml")
    with open(config_path, "r", encoding="utf-8") as f:
        config = yaml.safe_load(f)

    # 注册信号处理器
    signal.signal(signal.SIGTERM, _shutdown_handler)
    signal.signal(signal.SIGINT, _shutdown_handler)

    # 本地发件箱目录: 与 C++ 记录器约定一致(env ALARM_STORE_DIR 覆盖, 否则 <app>/alarm_store)
    store_dir = os.environ.get("ALARM_STORE_DIR") or \
        os.path.abspath(os.path.join(script_dir, "..", "..", "alarm_store"))
    os.makedirs(store_dir, exist_ok=True)

    print("[Sys] Unified uploader started (Multi-Threaded Mode)")
    print(f"[Sys] 本地告警发件箱: {store_dir}")

    dify_queue = config["redis"]["dify_queue"]

    # server 路径: 扫描本地发件箱补传(传成功即删), 不再走 Redis
    forwarder = OutboxForwarder(config, store_dir)
    dify_uploader = DifyUploader(config)

    t1 = threading.Thread(
        target=forwarder.run,
        name="OutboxForwarder",
        daemon=True,
    )
    # dify 路径: 保持原样(Redis 队列消费)
    t2 = threading.Thread(
        target=queue_worker,
        args=(config, dify_queue, dify_uploader),
        name="DifyWorker",
        daemon=True,
    )

    t1.start()
    t2.start()

    try:
        # 主线程等待关闭信号, 同时以有限超时 join worker 线程
        while t1.is_alive() or t2.is_alive():
            t1.join(timeout=1)
            t2.join(timeout=1)
    except KeyboardInterrupt:
        print("[Sys] Service stopped by user")
        _shutdown_event.set()

    print("[Sys] All workers stopped, exiting")
    sys.exit(0)


if __name__ == "__main__":
    main()
