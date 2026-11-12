"""Small servo moves around the current pose (run on the Pi while watching the robot).

uv run python tests/servo_steps.py STEP_DEG SPEED_DPS [--home] [--off]
"""

import argparse
import queue
import time

from stackchan_host import StackChanClient


def show(c: StackChanClient, label: str) -> None:
    s = c.call("servo.get")
    y, p = s["yaw"], s["pitch"]
    print(f"{label:16s} yaw {y['angle']:6.1f} (target {y['target']:6.1f}, load {y['load']:4d}, {y['temp_c']} C)"
          f"  pitch {p['angle']:6.1f} (target {p['target']:6.1f}, load {p['load']:4d}, {p['temp_c']} C)")


parser = argparse.ArgumentParser()
parser.add_argument("step", type=float)
parser.add_argument("speed", type=float)
parser.add_argument("--home", action="store_true", help="finish with servo.home")
parser.add_argument("--off", action="store_true", help="finish with servo.power off")
args = parser.parse_args()

with StackChanClient() as c:
    c.call("sys.subscribe", {"events": ["servo.fault"]})
    show(c, "start")
    if not c.call("servo.get")["yaw"]["torque"]:
        c.call("servo.power", {"on": True})
        time.sleep(1.0)
        show(c, "power on (hold)")
    s0 = c.call("servo.get")
    y0, p0 = s0["yaw"]["target"], s0["pitch"]["target"]
    pause = args.step / args.speed + 0.8
    moves = [
        ("yaw +", {"yaw": y0 + args.step}), ("yaw -", {"yaw": y0 - args.step}), ("yaw back", {"yaw": y0}),
        ("pitch +", {"pitch": min(85, p0 + args.step)}), ("pitch -", {"pitch": max(5, p0 - args.step)}),
        ("pitch back", {"pitch": p0}),
    ]
    for label, target in moves:
        c.call("servo.move", {**target, "speed": args.speed})
        time.sleep(pause * (2 if label.endswith("-") else 1))
        show(c, label)
    if args.home:
        c.call("servo.home")
        time.sleep(max(abs(y0), abs(p0 - 5)) / 60 + 1)
        show(c, "home")
    if args.off:
        c.call("servo.power", {"on": False})
        time.sleep(0.5)
        show(c, "power off")
    while True:
        try:
            print("EVENT", c.notifications.get(timeout=0.2))
        except queue.Empty:
            break
