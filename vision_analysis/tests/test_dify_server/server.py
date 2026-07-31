#!/usr/bin/env python3
"""
Dify 上报测试服务器 —— 模拟 Dify API，验证文件上传和工作流调用是否正常。

=== 模拟的 Dify API ===
  1. POST /v1/files/upload      multipart 文件上传，返回 {"id": "test-file-<uuid>"}, HTTP 201
  2. POST /v1/workflows/run     JSON 工作流调用，打印 inputs 并返回 {"data": {"status": "succeeded"}}, HTTP 200
  3. GET  /                     健康检查

=== 保存内容 ===
  received_files/   上传的原始文件（图片/视频），文件名包含请求序号便于对应
  received/         每次 API 请求的完整记录（headers + parsed body + 文件信息）

=== 用法 ===
  python3 server.py [--port PORT] [--host HOST]

  PORT  监听端口，默认 9201
  HOST  监听地址，默认 0.0.0.0

=== Profile 配置 ===
  Web 控制台 → 服务配置 → 新建 Profile（或修改 inspection_dify）：
    适配器: dify_workflow
    api_url: http://127.0.0.1:9201
    api_key: test-api-key-123
    超时:    120
"""

import argparse
import json
import os
import sys
import time
import uuid
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

SAVE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received")
FILES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received_files")
_seq_counter = 0


def _next_seq() -> int:
    global _seq_counter
    _seq_counter += 1
    return _seq_counter


class DifyTestHandler(BaseHTTPRequestHandler):
    server_start_time: str = ""

    def log_message(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {fmt % args}", flush=True)

    # ── helpers ──────────────────────────────────────────────

    def _print_divider(self, title: str = ""):
        print(f"\n{'─' * 60}", flush=True)
        if title:
            print(f"  {title}", flush=True)
            print(f"{'─' * 60}", flush=True)

    def _print_headers(self):
        for key in ("Authorization", "Content-Type", "X-Idempotency-Key"):
            val = self.headers.get(key)
            if val:
                masked = val if key != "Authorization" else val[:20] + ("..." if len(val) > 20 else "")
                print(f"  {key}: {masked}", flush=True)

    def _save_record(self, record: dict):
        os.makedirs(SAVE_DIR, exist_ok=True)
        ts = time.strftime("%Y%m%d_%H%M%S")
        seq = str(int(time.time() * 1000) % 100000)
        filename = os.path.join(SAVE_DIR, f"req_{ts}_{seq}.json")
        with open(filename, "w", encoding="utf-8") as f:
            json.dump(record, f, ensure_ascii=False, indent=2)
        return filename

    def _parse_multipart(self, content_type: str, body: bytes):
        """简易 multipart/form-data 解析，返回 (fields, files)。"""
        fields: dict = {}
        files: dict = {}

        if not content_type or b"boundary=" not in content_type.encode():
            return fields, files

        boundary = None
        for part in content_type.split(";"):
            part = part.strip()
            if part.startswith("boundary="):
                boundary = part[len("boundary="):].strip('"').encode()

        if not boundary:
            return fields, files

        parts = body.split(b"--" + boundary)
        for part in parts:
            if part in (b"", b"--", b"--\r\n"):
                continue

            header_end = part.find(b"\r\n\r\n")
            if header_end < 0:
                continue
            headers_block = part[:header_end].decode("utf-8", errors="replace")
            file_data = part[header_end + 4:]
            if file_data.endswith(b"\r\n"):
                file_data = file_data[:-2]

            disp_match = None
            disp_name = None
            disp_filename = None
            for line in headers_block.split("\r\n"):
                if line.lower().startswith("content-disposition:"):
                    disp_match = line
                if line.lower().startswith("content-type:"):
                    pass  # not needed for this mock

            if not disp_match:
                continue

            # parse name=
            for seg in disp_match.split(";"):
                seg = seg.strip()
                if seg.startswith("name="):
                    disp_name = seg[len("name="):].strip('"')
                if seg.startswith("filename="):
                    disp_filename = seg[len("filename="):].strip('"')

            if not disp_name:
                continue

            if disp_filename:
                files[disp_name] = {
                    "filename": disp_filename,
                    "data": file_data,
                    "size": len(file_data),
                }
            else:
                fields[disp_name] = file_data.decode("utf-8", errors="replace")

        return fields, files

    # ── handlers ─────────────────────────────────────────────

    def _handle_file_upload(self):
        content_type = self.headers.get("Content-Type", "")
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len) if content_len > 0 else b""

        fields, files = self._parse_multipart(content_type, body)

        seq = _next_seq()

        self._print_divider(f"[#{seq}] POST /v1/files/upload")
        self._print_headers()

        print(f"  Form fields: {json.dumps(fields, ensure_ascii=False)}", flush=True)

        saved_files = []
        for field_name, file_info in files.items():
            ext = os.path.splitext(file_info["filename"])[1] or ".bin"
            saved_name = f"seq{seq:04d}_{field_name}_{file_info['filename']}"
            saved_path = os.path.join(FILES_DIR, saved_name)
            os.makedirs(FILES_DIR, exist_ok=True)
            with open(saved_path, "wb") as f:
                f.write(file_info["data"])

            size_kb = file_info["size"] / 1024
            size_str = f"{size_kb:.1f} KB" if size_kb < 1024 else f"{size_kb / 1024:.2f} MB"
            print(f"  📁 {field_name}: {file_info['filename']} ({size_str}) -> {saved_name}", flush=True)
            saved_files.append({
                "field_name": field_name,
                "original_filename": file_info["filename"],
                "saved_as": saved_name,
                "size_bytes": file_info["size"],
            })

        if not files:
            print(f"  ⚠ 没有收到文件！", flush=True)

        file_ids = {}
        for field_name in files:
            fid = f"test-file-{uuid.uuid4().hex[:12]}"
            file_ids[field_name] = fid

        response_body = {"id": list(file_ids.values())[0] if file_ids else f"test-file-{uuid.uuid4().hex[:12]}"}

        record = {
            "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "seq": seq,
            "endpoint": "/v1/files/upload",
            "headers": dict(self.headers),
            "form_fields": fields,
            "files": saved_files,
            "response": response_body,
            "response_status": 201,
        }
        record_path = self._save_record(record)
        print(f"  -> 201, file_id={response_body['id']}", flush=True)
        print(f"  -> 记录: {os.path.basename(record_path)}", flush=True)

        self.send_response(201)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.end_headers()
        self.wfile.write(json.dumps(response_body, ensure_ascii=False).encode())

    def _handle_workflow_run(self):
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len) if content_len > 0 else b""
        body_str = body.decode("utf-8", errors="replace")

        seq = _next_seq()

        self._print_divider(f"[#{seq}] POST /v1/workflows/run")
        self._print_headers()

        try:
            parsed = json.loads(body_str)
        except (json.JSONDecodeError, ValueError):
            parsed = body_str

        print(f"\n  Payload:", flush=True)
        if isinstance(parsed, dict):
            for key, value in parsed.items():
                val_str = json.dumps(value, ensure_ascii=False)
                if len(val_str) > 200:
                    val_str = val_str[:200] + f"... ({len(val_str)} chars total)"
                print(f"    {key}: {val_str}", flush=True)
        else:
            print(f"    {str(parsed)[:300]}", flush=True)

        # 检查 inputs 中是否有文件引用
        if isinstance(parsed, dict) and "inputs" in parsed:
            inputs = parsed["inputs"]
            for k, v in inputs.items():
                if isinstance(v, dict) and "upload_file_id" in v:
                    ftype = v.get("type", "unknown")
                    transfer = v.get("transfer_method", "unknown")
                    fid = v.get("upload_file_id", "?")
                    print(f"  📎 {k}: type={ftype}, method={transfer}, file_id={fid}", flush=True)

        workflow_run_id = parsed.get("workflow_run_id", "unknown") if isinstance(parsed, dict) else "unknown"
        response_body = {
            "data": {
                "id": f"test-run-{uuid.uuid4().hex[:12]}",
                "workflow_run_id": workflow_run_id,
                "status": "succeeded",
                "outputs": {"result": "mock success"},
            }
        }

        record = {
            "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "seq": seq,
            "endpoint": "/v1/workflows/run",
            "headers": dict(self.headers),
            "body": parsed,
            "response": response_body,
            "response_status": 200,
        }
        record_path = self._save_record(record)
        print(f"  -> 200, status=succeeded", flush=True)
        print(f"  -> 记录: {os.path.basename(record_path)}", flush=True)

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.end_headers()
        self.wfile.write(json.dumps(response_body, ensure_ascii=False).encode())

    # ── dispatch ─────────────────────────────────────────────

    def do_POST(self):
        path = urlparse(self.path).path.rstrip("/")

        if path in ("/v1/files/upload", "/files/upload"):
            self._handle_file_upload()
        elif path in ("/v1/workflows/run", "/workflows/run"):
            self._handle_workflow_run()
        else:
            self._print_divider(f"POST {self.path} (unknown)")
            self.send_response(404)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            self.wfile.write(json.dumps({"error": "not found"}).encode())

    def do_GET(self):
        path = urlparse(self.path).path.rstrip("/")
        print(f"[{time.strftime('%H:%M:%S')}] GET {self.path}", flush=True)
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.end_headers()
        self.wfile.write(json.dumps({
            "server": "dify_test_server",
            "started_at": self.server_start_time,
            "files_saved": FILES_DIR,
            "records_saved": SAVE_DIR,
        }, ensure_ascii=False).encode())


def main():
    parser = argparse.ArgumentParser(description="Dify 上报测试服务器")
    parser.add_argument("--port", type=int, default=9201, help="监听端口 (默认 9201)")
    parser.add_argument("--host", type=str, default="0.0.0.0", help="监听地址 (默认 0.0.0.0)")
    args = parser.parse_args()

    start_time = time.strftime("%Y-%m-%d %H:%M:%S")
    DifyTestHandler.server_start_time = start_time

    server = HTTPServer((args.host, args.port), DifyTestHandler)

    os.makedirs(SAVE_DIR, exist_ok=True)
    os.makedirs(FILES_DIR, exist_ok=True)

    print(f"Dify 测试服务器已启动", flush=True)
    print(f"  地址: http://{args.host}:{args.port}", flush=True)
    print(f"  文件保存目录: {FILES_DIR}", flush=True)
    print(f"  记录保存目录: {SAVE_DIR}", flush=True)
    print(f"  启动时间: {start_time}", flush=True)
    print(f"  模拟端点:", flush=True)
    print(f"    POST /v1/files/upload   -> 201 + file_id", flush=True)
    print(f"    POST /v1/workflows/run  -> 200 + succeeded", flush=True)
    print(f"  按 Ctrl+C 停止", flush=True)
    print(f"{'=' * 60}", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止", flush=True)
        server.shutdown()


if __name__ == "__main__":
    main()
