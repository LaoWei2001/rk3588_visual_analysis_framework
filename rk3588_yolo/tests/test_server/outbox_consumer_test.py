#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
outbox_consumer_test.py — 本地发件箱消费端（end-to-end 联调用）

配合 server_queue_producer_test 使用，构成完整链路：
  server_queue_producer_test  (C++)   → 落盘 alarm_store/（带框图 + 原图 + .json）
  outbox_consumer_test.py     (本脚本) → 扫描 alarm_store/ → HTTP POST 业务服务器 → 传成功即删

★ 复用正式部署的转发逻辑（不复制）★
  本脚本 import 正式部署用的 service/upload/main.py 里的 OutboxForwarder：
  - 默认（持续运行）模式：直接调用 forwarder.run()，与线上逐字一致——含 重试/退避、
    送达即删、断网攒着、server_url 回落、payload 格式。
  - --once 模式：复用线上的逐条转发 forwarder._forward_one()（真正的 POST + 删除都是线上代码），
    只是外层换成"扫一轮就退出"的简单循环，不含 run() 的多轮退避。
  两种模式下"真正发出去"的那段都是线上代码，不会随版本漂移。

与正式部署的唯一区别：
  - 发件箱目录默认指向本脚本的同级目录 ./alarm_store/（正式是 <app>/alarm_store）
  - 只跑 server 这一条（OutboxForwarder）；dify 路径走 Redis，不在本脚本范围

用法：
  python3 outbox_consumer_test.py            # 持续运行（同正式部署），Ctrl+C 退出
  python3 outbox_consumer_test.py --once     # 把当前积压补传一轮后退出（脚本化快速验证）
  python3 outbox_consumer_test.py --store <dir>      # 指定发件箱目录（默认同级 alarm_store/）
  python3 outbox_consumer_test.py --config <yaml>    # 指定上报配置（默认仓库 service/upload/config.yaml）

服务器地址：取自 config.yaml 的 server.url——和 producer 留空时回落的默认地址是同一个。
"""

import argparse
import importlib.util
import os
import signal
import sys
from pathlib import Path

import yaml

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]                 # tests/test_server → tests → rk3588_yolo → 仓库根
PROD_MAIN = REPO_ROOT / "service" / "upload" / "main.py"
DEFAULT_CONFIG = REPO_ROOT / "service" / "upload" / "config.yaml"
DEFAULT_STORE = SCRIPT_DIR / "alarm_store"        # 同级目录


def load_prod_module():
    """加载正式部署的 service/upload/main.py，复用其 OutboxForwarder（保证逻辑 100% 一致）。"""
    if not PROD_MAIN.exists():
        sys.exit(f"[ERR] 找不到正式上报服务代码: {PROD_MAIN}\n"
                 f"      本脚本复用它的 OutboxForwarder，请在完整仓库内运行。")
    spec = importlib.util.spec_from_file_location("prod_upload", str(PROD_MAIN))
    mod = importlib.util.module_from_spec(spec)
    try:
        # 只执行模块级定义（类/全局变量）；main() 有 __main__ 守卫，import 时不会运行
        spec.loader.exec_module(mod)
    except Exception as e:
        sys.exit(f"[ERR] 加载 {PROD_MAIN} 失败（依赖未装？pip install redis requests pyyaml）: {e}")
    return mod


def load_config(path: str) -> dict:
    try:
        config = yaml.safe_load(Path(path).read_text(encoding="utf-8")) or {}
    except Exception as e:
        sys.exit(f"[ERR] 读取配置失败 {path}: {e}")
    server = config.get("server") or {}
    if not server.get("url"):
        sys.exit(f"[ERR] {path} 缺少 server.url（要发到哪个服务器）")
    server.setdefault("timeout", 15)
    config["server"] = server
    return config


def run_once(forwarder) -> None:
    """把当前积压补传一轮后返回（复用正式部署的 _list_pending / _forward_one）。"""
    pending = forwarder._list_pending()
    if not pending:
        print("[Outbox] 发件箱为空，没有待补传的记录。先跑 ./server_queue_producer_test 生成。")
        return
    sent = rejected = skipped = 0
    for jname in pending:
        r = forwarder._forward_one(jname)
        print(f"  [{r:8}] {jname}")
        if r == "sent":
            sent += 1
        elif r == "down":
            print("[Outbox] 平台不可达，停止本轮（数据仍在本地，体现断网攒着/重连补发）。")
            break
        elif r == "rejected":
            rejected += 1
        else:  # "skip"
            skipped += 1
    print(f"\n本轮: sent={sent} rejected={rejected} skipped={skipped} / 共 {len(pending)} 条")


def main() -> None:
    ap = argparse.ArgumentParser(description="本地发件箱消费端（复用正式 OutboxForwarder）")
    ap.add_argument("--store", default=os.environ.get("ALARM_STORE_DIR") or str(DEFAULT_STORE),
                    help="发件箱目录（默认同级 ./alarm_store/，可用 ALARM_STORE_DIR 覆盖）")
    ap.add_argument("--config", default=str(DEFAULT_CONFIG),
                    help="上报配置 config.yaml（默认仓库 service/upload/config.yaml）")
    ap.add_argument("--once", action="store_true",
                    help="把当前积压补传一轮后退出（默认持续运行，同正式部署）")
    args = ap.parse_args()

    prod = load_prod_module()
    config = load_config(args.config)
    store_dir = os.path.abspath(args.store)
    os.makedirs(store_dir, exist_ok=True)

    forwarder = prod.OutboxForwarder(config, store_dir)

    print("=" * 60)
    print("  本地发件箱消费端（逻辑复用 service/upload 的 OutboxForwarder）")
    print(f"  发件箱   : {store_dir}")
    print(f"  默认地址 : {config['server']['url']}")
    print(f"  模式     : {'单轮补传后退出 (--once)' if args.once else '持续运行 (Ctrl+C 退出)'}")
    print("=" * 60)

    if args.once:
        run_once(forwarder)
        return

    # 持续运行：与正式部署一致，靠 prod._shutdown_event 优雅退出
    def _stop(signum, frame):
        print(f"\n[Sys] 收到信号 {signum}，准备退出…")
        prod._shutdown_event.set()

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    forwarder.run()
    print("[Sys] 消费端已退出")


if __name__ == "__main__":
    main()
