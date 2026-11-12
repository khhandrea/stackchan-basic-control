"""Minimal PNG writer for display captures (standard library only)."""

from __future__ import annotations

import base64
import struct
import zlib


def rgb565be_to_png(data_b64: str, width: int, height: int) -> bytes:
    raw = base64.b64decode(data_b64)
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: none
        line = raw[y * width * 2 : (y + 1) * width * 2]
        for i in range(0, len(line), 2):
            v = (line[i] << 8) | line[i + 1]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
        + chunk(b"IEND", b"")
    )
