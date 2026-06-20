#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dify_queue_consumer_test.py — Dify 队列消费端（end-to-end 联调用）

配合 dify_queue_producer_test 使用，构成完整链路：
  dify_queue_producer_test    (C++)   → RPUSH 到 Redis dify_queue（带图 base64 + prompt + event_id）
  dify_queue_consumer_test.py (本脚本) → BLPOP dify_queue → 上传图片 + 触发 Dify 工作流

★ 复用正式部署的转发逻辑（不复制）★
  本脚本 import 正式部署用的 service/upload/main.py 里的 DifyUploader / queue_worker：
  - 默认（持续运行）模式：直接调用 queue_worker()，与线上逐字一致（blpop 阻塞消费 + 优雅退出）。
  - --once 模式：复用线上的 DifyUploader.upload()（真正的 文件上传 + 工作流触发都是线上代码），
    只是外层用 lpop 把当前积压消费完就退出，不走 queue_worker 的阻塞循环。
  两种模式下"真正上传/触发"的那段都是线上代码，不会随版本漂移。

与正式部署的唯一区别：只跑 dify 这一条（不含 server 的本地发件箱补传）。

★ 位置无关 ★
  dify 走 Redis（网络服务），不像 server 依赖本地 alarm_store/ 文件。只要能连到 C++ 生产者
  推送的那台 Redis（同 host:port、**同 db 0**、队列名 dify_queue），从任何机器/目录启动都能消费。
  注意 BLPOP 是抢占式：每条消息只被一个消费者取走；同时起多个会瓜分消息，不是广播。

用法：
  python3 dify_queue_consumer_test.py            # 持续运行（同正式部署 queue_worker），Ctrl+C 退出
  python3 dify_queue_consumer_test.py --once     # 把当前队列里的消息消费完后退出（脚本化快速验证）
  python3 dify_queue_consumer_test.py --config <yaml>   # 指定配置（默认仓库 service/upload/config.yaml）

Dify 地址：每条消息自带 dify_api_url/key 时用消息里的；留空才回落到 config.yaml 的 dify.api_url。
"""

import argparse
import importlib.util
import json
import signal
import sys
from pathlib import Path

import yaml

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]                 # tests/test_dify → tests → rk3588_yolo → 仓库根
PROD_MAIN = REPO_ROOT / "service" / "upload" / "main.py"
DEFAULT_CONFIG = REPO_ROOT / "service" / "upload" / "config.yaml"


def load_prod_module():
    """加载正式部署的 service/upload/main.py，复用其 DifyUploader / queue_worker（逻辑 100% 一致）。"""
    if not PROD_MAIN.exists():
        sys.exit(f"[ERR] 找不到正式上报服务代码: {PROD_MAIN}\n"
                 f"      本脚本复用它的 DifyUploader，请在完整仓库内运行。")
    spec = importlib.util.spec_from_file_location("prod_upload", str(PROD_MAIN))
    mod = importlib.util.module_from_spec(spec)
    try:
        # 只执行模块级定义（类/函数/全局变量）；main() 有 __main__ 守卫，import 时不会运行
        spec.loader.exec_module(mod)
    except Exception as e:
        sys.exit(f"[ERR] 加载 {PROD_MAIN} 失败（依赖未装？pip install redis requests pyyaml）: {e}")
    return mod


def load_config(path: str) -> dict:
    try:
        config = yaml.safe_load(Path(path).read_text(encoding="utf-8")) or {}
    except Exception as e:
        sys.exit(f"[ERR] 读取配置失败 {path}: {e}")
    if not config.get("dify"):
        sys.exit(f"[ERR] {path} 缺少 dify 配置节")
    config["dify"].setdefault("api_url", "")
    config["dify"].setdefault("api_key", "")
    config["dify"].setdefault("timeout", 120)
    redis_cfg = config.get("redis") or {}
    redis_cfg.setdefault("host", "localhost")
    redis_cfg.setdefault("port", 6379)
    redis_cfg.setdefault("db", 0)
    redis_cfg.setdefault("dify_queue", "dify_queue")
    config["redis"] = redis_cfg
    return config


def run_once(prod, config, queue_name, uploader) -> None:
    """把当前队列里的消息消费完后返回（复用正式 _connect_redis + DifyUploader.upload）。"""
    client = prod._connect_redis(config, queue_name)
    n = client.llen(queue_name)
    if n == 0:
        print(f"[Dify] 队列 {queue_name} 为空，没有待消费的消息。先跑 ./dify_queue_producer_test <图> 生成。")
        return
    print(f"[Dify] 队列 {queue_name} 当前 {n} 条，开始消费…")
    ok = fail = 0
    while True:
        raw = client.lpop(queue_name)     # 非阻塞，取空即停（只消费"当前"积压）
        if raw is None:
            break
        try:
            data = json.loads(raw)
        except json.JSONDecodeError as e:
            print(f"  [bad-json] 跳过一条: {e}")
            fail += 1
            continue
        good = uploader.upload(data)       # ← 正式部署的转发逻辑
        print(f"  [{'ok' if good else 'FAIL'}] event_id={data.get('event_id', '?')}")
        ok += int(good)
        fail += int(not good)
    print(f"\n本轮: success={ok} fail={fail}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Dify 队列消费端（复用正式 DifyUploader / queue_worker）")
    ap.add_argument("--config", default=str(DEFAULT_CONFIG),
                    help="上报配置 config.yaml（默认仓库 service/upload/config.yaml）")
    ap.add_argument("--once", action="store_true",
                    help="把当前队列里的消息消费完后退出（默认持续运行，同正式部署）")
    args = ap.parse_args()

    prod = load_prod_module()
    config = load_config(args.config)
    queue_name = config["redis"]["dify_queue"]
    uploader = prod.DifyUploader(config)

    print("=" * 60)
    print("  Dify 队列消费端（逻辑复用 service/upload 的 DifyUploader）")
    print(f"  Redis    : {config['redis']['host']}:{config['redis']['port']} db={config['redis']['db']}")
    print(f"  队列     : {queue_name}")
    print(f"  默认地址 : {config['dify']['api_url'] or '(未配置，仅靠消息自带地址)'}")
    print(f"  模式     : {'消费完当前积压后退出 (--once)' if args.once else '持续运行 (Ctrl+C 退出)'}")
    print("=" * 60)

    if args.once:
        run_once(prod, config, queue_name, uploader)
        return

    # 持续运行：直接用正式部署的 queue_worker（与线上行为完全一致），靠 _shutdown_event 优雅退出
    def _stop(signum, frame):
        print(f"\n[Sys] 收到信号 {signum}，准备退出…")
        prod._shutdown_event.set()

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    prod.queue_worker(config, queue_name, uploader)
    print("[Sys] 消费端已退出")


if __name__ == "__main__":
    main()
