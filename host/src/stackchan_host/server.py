"""Web dashboard server: bridges a browser to the device over HTTP (standard library only).

Endpoints:
- ``GET /``: dashboard page
- ``POST /api/call``: ``{"method", "params", "timeout"}`` → ``{"result": ...}`` or ``{"error": {"code", "message"}}``
- ``GET /api/events``: Server-Sent Events; each message is a device notification
  (``notify.event`` / ``notify.stream`` / ``notify.log`` / ``{"raw": ...}``), ``{"bridge": {"connected": bool}}``,
  or ``{"rpc": {"dir": "tx" | "rx", "msg": {...}}}`` for every request sent to and response received from the device
  (long strings such as base64 images abbreviated)

The serial port is reopened automatically after it fails (device reboot, power off, cable).
"""

from __future__ import annotations

import json
import queue
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources
from typing import Any

import serial

from .client import StackChanClient, RpcError

_KEEPALIVE_S = 15
_LISTENER_QUEUE = 2000
_MAX_TIMEOUT_S = 30
_TRACE_STRING_MAX = 120


def _abbreviate(value: Any) -> Any:
    if isinstance(value, str) and len(value) > _TRACE_STRING_MAX:
        return f"{value[:40]}… ({len(value)} chars)"
    if isinstance(value, dict):
        return {k: _abbreviate(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_abbreviate(v) for v in value]
    return value


class Bridge:
    """Owns the serial client, reconnects it, and fans notifications out to SSE listeners."""

    def __init__(self, port: str):
        self._port = port
        self._client: StackChanClient | None = None
        self._listeners: set[queue.Queue[dict[str, Any]]] = set()
        self._lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()

    @property
    def connected(self) -> bool:
        client = self._client
        return client is not None and client.alive

    def call(self, method: str, params: dict[str, Any] | None, timeout: float) -> Any:
        client = self._client
        if client is None or not client.alive:
            raise ConnectionError("device not connected")
        try:
            return client.call(method, params, timeout=timeout)
        except serial.SerialException as exc:
            raise ConnectionError(str(exc)) from exc

    def listen(self) -> queue.Queue[dict[str, Any]]:
        q: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=_LISTENER_QUEUE)
        with self._lock:
            self._listeners.add(q)
        return q

    def unlisten(self, q: queue.Queue[dict[str, Any]]) -> None:
        with self._lock:
            self._listeners.discard(q)

    def _broadcast(self, message: dict[str, Any]) -> None:
        with self._lock:
            listeners = list(self._listeners)
        for q in listeners:
            try:
                q.put_nowait(message)
            except queue.Full:
                pass  # slow browser; drop rather than block the device reader

    def _trace(self, direction: str, message: dict[str, Any]) -> None:
        self._broadcast({"rpc": {"dir": direction, "msg": _abbreviate(message)}})

    def _run(self) -> None:
        while True:
            client = self._client
            if client is None or not client.alive:
                if client is not None:
                    client.close()
                    self._client = None
                    self._broadcast({"bridge": {"connected": False}})
                try:
                    client = StackChanClient(self._port)
                except (serial.SerialException, OSError):
                    time.sleep(1)
                    continue
                client.trace = self._trace
                self._client = client
                self._broadcast({"bridge": {"connected": True}})
                continue
            try:
                message = client.notifications.get(timeout=0.5)
            except queue.Empty:
                continue
            self._broadcast(message)


def _make_handler(bridge: Bridge, page: bytes) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
            pass  # quiet; the dashboard polls often

        def _send(self, status: int, body: bytes, content_type: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, status: int, value: Any) -> None:
            self._send(status, json.dumps(value, ensure_ascii=False).encode(), "application/json; charset=utf-8")

        def do_GET(self) -> None:
            if self.path in ("/", "/index.html"):
                self._send(HTTPStatus.OK, page, "text/html; charset=utf-8")
            elif self.path == "/api/events":
                self._events()
            else:
                self._json(HTTPStatus.NOT_FOUND, {"error": {"code": 404, "message": "not found"}})

        def do_POST(self) -> None:
            if self.path != "/api/call":
                self._json(HTTPStatus.NOT_FOUND, {"error": {"code": 404, "message": "not found"}})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                request = json.loads(self.rfile.read(length) or b"{}")
                method = request["method"]
                params = request.get("params") or None
                timeout = min(float(request.get("timeout") or 5), _MAX_TIMEOUT_S)
                if not isinstance(method, str) or (params is not None and not isinstance(params, dict)):
                    raise ValueError
            except (ValueError, KeyError, TypeError):
                self._json(HTTPStatus.BAD_REQUEST, {"error": {"code": 400, "message": "bad request"}})
                return
            try:
                self._json(HTTPStatus.OK, {"result": bridge.call(method, params, timeout)})
            except RpcError as exc:
                self._json(HTTPStatus.OK, {"error": {"code": exc.code, "message": exc.message}})
            except TimeoutError as exc:
                self._json(HTTPStatus.GATEWAY_TIMEOUT, {"error": {"code": 504, "message": str(exc)}})
            except ConnectionError as exc:
                self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"error": {"code": 503, "message": str(exc)}})

        def _events(self) -> None:
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            q = bridge.listen()
            try:
                self._write_event({"bridge": {"connected": bridge.connected}})
                while True:
                    try:
                        message = q.get(timeout=_KEEPALIVE_S)
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                        continue
                    self._write_event(message)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                bridge.unlisten(q)
                self.close_connection = True

        def _write_event(self, message: dict[str, Any]) -> None:
            data = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
            self.wfile.write(f"data: {data}\n\n".encode())
            self.wfile.flush()

    return Handler


def serve(port: str, host: str, http_port: int) -> None:
    page = resources.files("stackchan_host").joinpath("dashboard.html").read_bytes()
    bridge = Bridge(port)
    server = ThreadingHTTPServer((host, http_port), _make_handler(bridge, page))
    server.daemon_threads = True
    print(f"dashboard on http://{host}:{http_port}/ (device {port})", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
