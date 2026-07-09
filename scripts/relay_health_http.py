#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import subprocess


CHECK_BIN = os.environ.get("RELAY_HEALTH_CHECK_BIN", "/usr/local/bin/relay_health_check")
HTTP_HOST = os.environ.get("RELAY_HEALTH_HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("RELAY_HEALTH_HTTP_PORT", "8080"))
CHECK_TIMEOUT_SECONDS = float(os.environ.get("RELAY_HEALTH_HTTP_TIMEOUT_SECONDS", "10"))


def _to_text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def _detail_text(stdout, stderr, returncode):
    chunks = []

    stdout = _to_text(stdout)
    stderr = _to_text(stderr)

    stdout_lines = stdout.strip().splitlines()
    if stdout_lines and stdout_lines[0] in ("ok", "error"):
        stdout_lines = stdout_lines[1:]

    stdout_details = "\n".join(stdout_lines).strip()
    stderr_details = stderr.strip()

    if stdout_details:
        chunks.append(stdout_details)
    if stderr_details:
        chunks.append(stderr_details)

    if not chunks:
        chunks.append(f"relay_health_check exited with status {returncode}")

    return "\n".join(chunks)


def run_health_check():
    try:
        result = subprocess.run(
            [CHECK_BIN],
            capture_output=True,
            check=False,
            text=True,
            timeout=CHECK_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        details = _detail_text(stdout, stderr, "timeout")
        details = f"relay_health_check timed out after {CHECK_TIMEOUT_SECONDS:g} seconds\n{details}"
        return HTTPStatus.SERVICE_UNAVAILABLE, f"error\n\n{details.strip()}\n"
    except OSError as exc:
        return HTTPStatus.SERVICE_UNAVAILABLE, f"error\n\nfailed to run {CHECK_BIN}: {exc}\n"

    if result.returncode == 0:
        return HTTPStatus.OK, "ok\n"

    details = _detail_text(result.stdout, result.stderr, result.returncode)
    return HTTPStatus.SERVICE_UNAVAILABLE, f"error\n\n{details.strip()}\n"


class RelayHealthHandler(BaseHTTPRequestHandler):
    server_version = "relay-health-http/1.0"

    def do_GET(self):
        status, body = run_health_check()
        body_bytes = body.encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body_bytes)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body_bytes)

    def do_HEAD(self):
        status, body = run_health_check()
        body_bytes = body.encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body_bytes)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def log_message(self, format, *args):
        print(f"{self.address_string()} - {format % args}", flush=True)


if __name__ == "__main__":
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), RelayHealthHandler)
    print(f"relay health HTTP server listening on {HTTP_HOST}:{HTTP_PORT}", flush=True)
    server.serve_forever()
