# StackChan Host Client

Python client for the StackChan general firmware ([PROTOCOL](../firmwares/general/PROTOCOL.md)). Managed with uv; runs on the Raspberry Pi, where the StackChan is connected.

## Run from the Build PC

`pi.sh` copies this directory to `~/stackchan/host` on the Pi (tar over ssh) and runs the CLI there with `uv run`:

```sh
./pi.sh ping                       # round-trip sys.ping
./pi.sh call sys.info              # call a method, print the result
./pi.sh call servo.move '{"yaw":30,"pitch":45}'
./pi.sh listen -s 10               # print notifications for 10 s (Ctrl-C to stop without -s)
```

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
