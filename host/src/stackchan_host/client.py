"""JSON-RPC client for the general firmware over USB-Serial/JTAG (see firmwares/general/PROTOCOL.md)."""

from __future__ import annotations

import itertools
import json
import queue
import threading
from typing import Any, Callable

import serial

DEFAULT_PORT = "/dev/ttyACM0"


class RpcError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class StackChanClient:
    """Sends requests and collects notifications.

    Notifications are queued as dicts: ``{"method": "notify.event", "params": {...}}``.
    Non-JSON lines (ROM boot text, panic output) are queued as ``{"raw": "<line>"}``.

    The port is opened with DTR and RTS both asserted and never toggled: on the ESP32-S3
    USB-Serial/JTAG, RTS high with DTR low resets the chip.
    """

    def __init__(self, port: str = DEFAULT_PORT):
        self._serial = serial.Serial(port, 115200, timeout=0.2, exclusive=True)
        self._ids = itertools.count(1)
        self._pending: dict[int, queue.Queue[dict[str, Any]]] = {}
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self.notifications: queue.Queue[dict[str, Any]] = queue.Queue()
        # Optional observer of requests sent ("tx") and responses received ("rx"), called from I/O threads
        self.trace: Callable[[str, dict[str, Any]], None] | None = None
        self._closed = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    @property
    def alive(self) -> bool:
        """False once the serial port failed (e.g. the device rebooted or powered off)."""
        return self._reader.is_alive() and not self._closed.is_set()

    def close(self) -> None:
        self._closed.set()
        self._reader.join(timeout=1)
        self._serial.close()

    def __enter__(self) -> StackChanClient:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def call(self, method: str, params: dict[str, Any] | None = None, timeout: float = 2.0) -> Any:
        """Sends a request and returns its result. Raises RpcError or TimeoutError."""
        request_id = next(self._ids)
        slot: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=1)
        with self._lock:
            self._pending[request_id] = slot
        try:
            self._send({"id": request_id, "method": method, **({"params": params} if params else {})})
            try:
                response = slot.get(timeout=timeout)
            except queue.Empty:
                raise TimeoutError(f"no response to {method} within {timeout} s") from None
        finally:
            with self._lock:
                self._pending.pop(request_id, None)
        if "error" in response:
            error = response["error"]
            raise RpcError(error.get("code", 0), error.get("message", ""))
        return response.get("result")

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        """Sends a request without id (no response)."""
        self._send({"method": method, **({"params": params} if params else {})})

    def _send(self, message: dict[str, Any]) -> None:
        if self.trace is not None:
            self.trace("tx", message)
        line = json.dumps(message, separators=(",", ":")) + "\n"
        with self._write_lock:
            self._serial.write(line.encode())
            self._serial.flush()

    def _read_loop(self) -> None:
        buffer = b""
        while not self._closed.is_set():
            try:
                # Wait for one byte (up to the timeout), then take whatever else has arrived.
                chunk = self._serial.read(max(1, self._serial.in_waiting))
            except serial.SerialException as exc:
                self.notifications.put({"raw": f"<serial error: {exc}>"})
                return
            if not chunk:
                continue
            buffer += chunk
            *lines, buffer = buffer.split(b"\n")
            for raw in lines:
                self._dispatch(raw.decode(errors="replace").rstrip("\r"))

    def _dispatch(self, line: str) -> None:
        if not line.startswith("{"):
            if line:
                self.notifications.put({"raw": line})
            return
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            self.notifications.put({"raw": line})
            return
        if "id" in message and ("result" in message or "error" in message):
            with self._lock:
                slot = self._pending.get(message["id"])
            if self.trace is not None:
                self.trace("rx", message)
            if slot is not None:
                slot.put(message)
                return
        self.notifications.put(message)
