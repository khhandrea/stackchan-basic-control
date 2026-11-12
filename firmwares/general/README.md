# StackChan General Firmware

General-purpose firmware for StackChan (M5Stack K151). It runs standalone, needs no cloud server, and is controlled from a host (Raspberry Pi 3) over USB serial.

## Communication

- Link: USB-C (body base port), USB-Serial/JTAG
- Message kinds

| Kind | Direction | Purpose |
|---|---|---|
| Request / response | Host → Device → Host | Commands and on-demand reads |
| `notify.event` | Device → Host | Pushed events (subscribed by type, sequence-numbered) |
| `notify.stream` | Device → Host | Periodic values at a requested rate (off by default) |
| `notify.log` | Device → Host | Human-readable debug text |

- Format: JSON-RPC 2.0, one JSON object per line. See [PROTOCOL.md](PROTOCOL.md).

## Events

| Event | Description |
|---|---|
| Boot | Firmware version, reset reason, MAC, free memory |
| Head touch | Touch start / end, tap, long press (3 zones) |
| Screen touch | Tap, long press, coordinates |
| Button | Power button press |
| Motion | Lifted, shaken, tilted, fallen |
| Proximity | Near / far threshold crossing |
| Power | USB attach / detach, charge start / done / stop, low battery |
| Servo fault | Overheat, overload, stall, bus error (servo power cut automatically) |
| Modal result | Button pressed or timeout |
| Error | Sensor init failure, I2C error, low memory |

## Reads

| Read | Values |
|---|---|
| `status` | Uptime, battery % / V / mA, charging, VBUS, chip temperature, free memory |
| `touch` | Head touch zones, screen touch; event counters and last event time |
| `button` | Power button state; event counter and last event time |
| `imu` | Accelerometer, gyroscope, magnetometer, tilt, motion state |
| `light` | Ambient light (lux), proximity |
| `servo` | Current / target angles, temperature, voltage, load, power state |
| `led` | Color of each of the 12 LEDs |
| `audio` | Volume, ambient sound level (RMS) |
| `time` | RTC time |
| `face` | Current expression and face parameters |
| `modal` | Current modal message |
| `snapshot` | Single camera JPEG (base64) |
| `info` | Firmware version, build date, MAC |

## Controls

### Display

| Control | Description |
|---|---|
| Expression | Neutral, happy, angry, sad, doubt, sleepy |
| Face parameters | Eye position, openness, rotation, size; mouth openness, rotation |
| Decorators | Heart, angry mark, sweat, dizzy, shy |
| Auto motion | Blink and breathing on / off |
| Modal message | Title and body, timeout or persistent, buttons (OK / Yes-No), close |
| Screen | Brightness, on / off |

### Servos

| Control | Description |
|---|---|
| Move | Yaw (-128–128°) and pitch (5–85°) angle, by speed or spring parameters |
| Rotate | Continuous yaw rotation in wheel mode (velocity -1000 to 1000) |
| Home | Return to neutral pose |
| Power | Servo power on (hold) / off (free) |
| Limits | Pitch clamped to 5–85° |

### LEDs

| Control | Description |
|---|---|
| All | Set all 12 LEDs to one color |
| Left / right | Per-side color with transition time |
| Single | Set LED 0–11 individually |
| Brightness | Global scale 0–100 %; final channel values capped at 168 |

### Speaker

| Control | Description |
|---|---|
| Volume | 0–100 |
| Beep | Frequency and short duration |

### System

| Control | Description |
|---|---|
| Subscribe | Enable / disable event types |
| Stream | Start / stop periodic values at a given rate |
| Log | Log level, periodic power log interval |
| Camera | Resolution |
| Time | Set RTC from host |
| Ping | Link check |
| Reboot / power off | Restart or shut down |

## Build

| Item | Value |
|---|---|
| Target | ESP32-S3 (M5Stack CoreS3 in StackChan K151) |
| Framework | ESP-IDF v5.5.4 |
| Flash / PSRAM | 16 MB / 8 MB |
| Build host | Arch Linux PC ([build environment](../../documents/build-environment.md)) |
| Flash host | Raspberry Pi 3 (esptool over USB) |
| Version | `PROJECT_VER` in `CMakeLists.txt` (0.1.0) |

### Layout

| Path | Description |
|---|---|
| `sdkconfig.defaults` | Flash QIO 16 MB, quad PSRAM 80 MHz (as official firmware), console on USB-Serial/JTAG |
| `partitions.csv` | `nvs` and `phy_init` at the factory offsets, single 4 MB `factory` app |
| `main/link.*` | USB-Serial/JTAG line I/O (`hostlink`) |
| `main/notify.*` | Message output, `notify.event`, ESP_LOG → `notify.log` |
| `main/rpc.*` | JSON-RPC dispatch and error codes |
| `main/methods_*.cpp` | Method groups |
| `tools/flash.sh` | Copy images to the Pi and flash with esptool |
| `tools/restore-factory.sh` | Write the factory backup back from the Pi |

### Workflow

From this directory:

```sh
get_idf && idf.py build        # 1. build on the PC
tools/flash.sh                 # 2-3. copy to the Pi, check the port is free, flash
../../host/pi.sh ping          # 4. test with the host client on the Pi
```

- `tools/flash.sh` refuses to flash while another process holds the serial port (e.g. the logger in `~/stackchan-logs`)
- Host client: [host/](../../host/)

### Status

| Area | State |
|---|---|
| Link, JSON-RPC, errors, `notify.log` | Implemented |
| `sys.ping`, `sys.info`, `sys.reboot`, `rpc.list`, `boot` event | Implemented |
| Everything else in [PROTOCOL.md](PROTOCOL.md) | Not yet |

### Restore

`tools/restore-factory.sh` writes the full factory backup (verified by SHA256) to return to the official firmware.
