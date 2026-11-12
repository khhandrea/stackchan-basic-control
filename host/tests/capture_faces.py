"""Capture every expression, the decorators and a modal to PNG files (display check).

Run on the Pi: uv run python tests/capture_faces.py OUT_DIR
"""

import sys
import time
from pathlib import Path

from stackchan_host import StackChanClient
from stackchan_host.image import rgb565be_to_png

out = Path(sys.argv[1] if len(sys.argv) > 1 else "captures")
out.mkdir(parents=True, exist_ok=True)


def cap(c: StackChanClient, name: str) -> None:
    time.sleep(0.1)
    f = c.call("display.capture", timeout=10)
    (out / f"{name}.png").write_bytes(rgb565be_to_png(f["data"], f["width"], f["height"]))


with StackChanClient() as c:
    c.call("face.set_auto", {"blink": False, "breath": False})
    for e in ["neutral", "happy", "angry", "sad", "doubt", "sleepy"]:
        c.call("face.set_expression", {"name": e})
        cap(c, e)
    c.call("face.set_expression", {"name": "neutral"})
    decorators = ["heart", "angry", "sweat", "shy"]
    for d in decorators:
        c.call("face.set_decorator", {"name": d, "on": True})
    c.call("face.set_params", {"eye_x": 0.8, "eye_y": -0.5, "mouth_open": 0.7, "mouth_rot": 10, "eye_size": 1.2})
    cap(c, "decorators")
    for d in decorators:
        c.call("face.set_decorator", {"name": d, "on": False})
    c.call("face.set_params", {"eye_x": 0, "eye_y": 0, "mouth_open": 0, "mouth_rot": 0, "eye_size": 1})
    c.call("face.set_decorator", {"name": "dizzy", "on": True})
    cap(c, "dizzy")
    c.call("face.set_decorator", {"name": "dizzy", "on": False})
    c.call("modal.show", {
        "title": "알림 Notice",
        "body": "안녕하세요!. This is a long body text that should wrap across several lines nicely.",
        "buttons": "yes_no",
    })
    cap(c, "modal")
    c.call("modal.close")
    c.call("face.set_auto", {"blink": True, "breath": True})
print(out)
