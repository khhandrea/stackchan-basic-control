# StackChan Host Client

Python client for the StackChan general firmware ([PROTOCOL](../firmwares/general/PROTOCOL.md)). Managed with uv; runs on the Raspberry Pi, where the StackChan is connected.

## Run from the Build PC

`pi.sh` copies this directory to `~/stackchan/host` on the Pi (tar over ssh) and runs the CLI there with `uv run`:

```sh
./pi.sh ping                       # round-trip sys.ping
./pi.sh call sys.info              # call a method, print the result
./pi.sh call servo.move '{"yaw":30,"pitch":45}'
./pi.sh listen -s 10               # print notifications for 10 s (Ctrl-C to stop without -s)
./pi.sh capture /home/hwanhee/screen.png   # rendered screen as PNG, saved on the Pi
```

## Web Dashboard

`stackchan serve` runs a web server on the Pi (standard library only) that bridges a browser to the device:

```sh
./pi.sh serve                      # from the build PC; Ctrl-C stops it
# open http://rpi3.local:8080/ (or the Pi's IP) in a browser on the same network
```

| Item | Description |
|---|---|
| Page | Every method, event and stream: status, face, modal, screen capture, LEDs, servos, audio, camera, IMU, touch, light, RTC, event / log viewers, raw RPC console |
| `POST /api/call` | `{"method", "params", "timeout"}` → `{"result"}` or `{"error": {"code", "message"}}` |
| `GET /api/events` | Server-Sent Events: device notifications, `{"bridge": {"connected": bool}}`, and `{"rpc": {"dir": "tx"/"rx", "msg"}}` for every request / response exchanged with the device |
| Options | `--host` (default `0.0.0.0`), `--http-port` (default 8080) |

- The serial port is reopened automatically after it fails; the page re-applies its event subscriptions and streams on connect and on `boot`.
- No authentication: anyone who can reach the port can control the device (including power off). Use `--host 127.0.0.1` with an SSH tunnel on untrusted networks.
- The server holds the serial port; stop it before running other `stackchan` commands or `tools/flash.sh`.

## Tests

| Script | Purpose |
|---|---|
| `tests/capture_faces.py OUT_DIR` | Captures every expression, the decorators and a modal as PNG (run on the Pi) |

## Run on the Pi

```sh
cd ~/stackchan/host && uv run stackchan ping
```

## Library

```python
from stackchan_host import StackChanClient

with StackChanClient("/dev/ttyACM0") as stackchan:
    print(stackchan.call("sys.info"))
    stackchan.call("sys.subscribe", {"events": ["touch"]})
    print(stackchan.notifications.get())
```

## Serial Port Notes

- The port is opened with DTR and RTS both asserted and never toggled. On the ESP32-S3 USB-Serial/JTAG, RTS high with DTR low resets the chip; opening this way was verified not to reset the device.
- The port is opened exclusively; stop other readers (e.g. `~/stackchan-logs` logger) first.
- Non-JSON lines (ROM boot text, panics) are delivered as `{"raw": "..."}`.
