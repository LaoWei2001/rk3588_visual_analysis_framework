#!/usr/bin/env python3
"""
简易 HTTP 上报测试服务器 — 用于验证框架上报功能是否正常工作。

=== 功能说明 ===
这个服务器只做"照单全收 + 全量打印"，不实现真实的业务逻辑：
  1. 接收任何路径的 POST 请求，不做路由过滤
  2. 终端打印关键 Header 和完整 JSON body
  3. 检查 invadeFlag 字段，仅打印提示（不实际解码 Base64 或保存图片文件）
  4. 每次请求合并 headers + body 保存为一份 JSON 到同级目录 received/
  5. 固定返回 {"msg": "操作成功", "code": 200}

=== 与真实服务器的区别 ===
  真实服务器 (192.168.2.22:9200):   invadeFlag=1 时解码 Base64 保存图片，可能有去重逻辑
  测试服务器:                       invadeFlag=1 时只打印"保存图片"，不做实际操作

=== 用法 ===
  python3 server.py [--port PORT]

  PORT  监听端口，默认 9200（与真实服务器一致，方便复用同一 Profile）

=== Profile 配置 ===
  Web 控制台 → 服务配置 → 新建 Profile：
    适配器: http
    URL:    http://127.0.0.1:9200/api/objectInvadeDet
    超时:   15
"""

import argparse
import json
import os
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

SAVE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received")


class UploadTestHandler(BaseHTTPRequestHandler):
    server_start_time: str = ""

    def log_message(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {fmt % args}", flush=True)

    def _save(self, headers: dict, body_str: str):
        os.makedirs(SAVE_DIR, exist_ok=True)
        ts = time.strftime("%Y%m%d_%H%M%S")
        seq = str(int(time.time() * 1000) % 100000)

        try:
            parsed_body = json.loads(body_str)
        except (json.JSONDecodeError, ValueError):
            parsed_body = body_str

        record = {
            "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "path": self.path,
            "headers": headers,
            "body": parsed_body,
        }

        filename = os.path.join(SAVE_DIR, f"req_{ts}_{seq}.json")
        with open(filename, "w", encoding="utf-8") as f:
            json.dump(record, f, ensure_ascii=False, indent=2)

    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len) if content_len > 0 else b""
        body_str = body.decode("utf-8", errors="replace")
        headers = dict(self.headers)

        print(f"\n{'='*60}", flush=True)
        print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] POST {self.path}", flush=True)
        print(f"{'='*60}", flush=True)

        for key in ("X-Idempotency-Key", "Content-Type", "Authorization"):
            val = headers.get(key)
            if val:
                print(f"  {key}: {val}", flush=True)

        print(f"\n  Body ({content_len} bytes):", flush=True)
        try:
            parsed = json.loads(body_str)
            for line in json.dumps(parsed, ensure_ascii=False, indent=2).splitlines():
                print(f"  {line}", flush=True)
            if "invadeFlag" in parsed:
                flag = parsed["invadeFlag"]
                label = "保存图片" if flag == 1 else "不保存图片"
                print(f"\n  *** invadeFlag={flag} -> {label} ***", flush=True)
        except (json.JSONDecodeError, ValueError):
            print(f"  {body_str}", flush=True)

        print(f"{'='*60}\n", flush=True)

        self._save(headers, body_str)

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.end_headers()
        self.wfile.write(json.dumps({"msg": "操作成功", "code": 200}, ensure_ascii=False).encode())

    def do_GET(self):
        print(f"[{time.strftime('%H:%M:%S')}] GET {self.path}", flush=True)
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.end_headers()
        self.wfile.write(json.dumps({
            "server": "upload_test_server",
            "started_at": self.server_start_time,
        }, ensure_ascii=False).encode())


def main():
    parser = argparse.ArgumentParser(description="简易 HTTP 上报测试服务器")
    parser.add_argument("--port", type=int, default=9200, help="监听端口 (默认 9200)")
    args = parser.parse_args()

    start_time = time.strftime("%Y-%m-%d %H:%M:%S")
    UploadTestHandler.server_start_time = start_time

    server = HTTPServer(("0.0.0.0", args.port), UploadTestHandler)

    os.makedirs(SAVE_DIR, exist_ok=True)

    print(f"上传测试服务器已启动", flush=True)
    print(f"  地址: http://0.0.0.0:{args.port}", flush=True)
    print(f"  保存目录: {SAVE_DIR}", flush=True)
    print(f"  启动时间: {start_time}", flush=True)
    print(f"  按 Ctrl+C 停止", flush=True)
    print(f"{'='*60}", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止", flush=True)
        server.shutdown()


if __name__ == "__main__":
    main()
