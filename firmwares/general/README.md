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
| Motion | Lifted, shaken, nudged (light shake), tilted, fallen |
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
| `button` | Power button state and last press time |
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
| Expression | Neutral, happy, unhappy, angry, sad, sleepy |
| Face parameters | Eye position, openness; mouth openness, rotation, size |
| Decorators | Heart, angry mark, sweat, shy, question mark |
| Auto motion | Blink, random gaze (saccades), reactions to head touch / shake / motion; on / off |
| Modal message | Title and body, timeout or persistent, buttons (OK / Yes-No), close |
| Screen | Brightness, on / off |

### Servos

| Control | Description |
|---|---|
| Move | Yaw (-128–128°) and pitch (5–85°) angle starting at a minimum speed (default 20 °/s), accelerating to a maximum speed (default 90 °/s) and slowing back down near the target (10–180 °/s) |
| Rotate | Continuous yaw rotation in wheel mode (velocity -1000 to 1000) |
| Home | Return to neutral pose |
| Power | Servo power on (hold) / off (free); on automatically at boot and after idle, off from idle level 1 |
| Limits | Pitch clamped to 5–85° |

### LEDs

| Control | Description |
|---|---|
| All | Set all 12 LEDs to one color |
| Left / right | Per-side color with transition time |
| Single | Set LED 0–11 individually |
| Brightness | Global scale 0–100 %; final channel values capped at 168 |
| Effect | Rotating rainbow gradient (`led.set_effect`) |

### Speaker

| Control | Description |
|---|---|
| Volume | 0–100 |
| Beep | Frequency, duration (default 250 ms), wave (sine / square / triangle / ramp, default ramp) |

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

## Idle

With no interaction (state-changing command, touch, button): 10 min sleepy face and servo power off, 30 min backlight dimmed to 64, 60 min power save (screen off, LED 0 blinking red). Any interaction restores the previous state; `idle.set_level` jumps to a level. See [PROTOCOL](PROTOCOL.md#idle).

## Battery

On USB the default storage charge mode keeps the battery between 40 % and 60 % (`power.set_charge_mode` `full` for normal charging).

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
| `main/idf_component.yml`, `dependencies.lock` | Managed components: `esp_lcd_ili9341`, `esp_codec_dev`, `esp32-camera` |
| `main/link`, `notify`, `rpc`, `events`, `streams`, `poller` | USB line I/O, JSON-RPC dispatch, events, streams, polling task |
| `main/i2c_bus`, `board` | Internal I2C bus; AW9523 setup, LCD reset, panel detection |
| `main/power` | AXP2101, INA226, chip temperature, power key: `sys.status`, `sys.power_off`, `button.get`, `log.*` |
| `main/lcd`, `gfx`, `face`, `modal`, `display` | LCD driver, band renderer and 16 px font, face, modal, 30 fps display task |
| `main/touch` | FT6336 screen touch, Si12T head touch |
| `main/imu`, `light`, `rtc` | BMI270 + BMM150 and motion events, LTR-553, BM8563 |
| `main/led` | 12 RGB LEDs via the PY32L020 |
| `main/audio` | AW88298 speaker, ES7210 microphones |
| `main/camera` | GC0308 snapshots |
| `main/idle` | Idle stages (sleepy, dim, power save) and wake on interaction |
| `main/react` | Reactions to head touch, shake and motion |
| `main/methods_sys`, `methods_debug` | `sys.ping` / `info` / `reboot`, `rpc.list`; debug `i2c.*` |
| `main/drivers/bmi270_config.c` | Bosch BMI270 config blob (BSD-3-Clause) |
| `main/assets/font16.fnt` | 16 px font (Noto Sans Mono CJK KR, OFL), generated by `tools/gen_font.py` |
| `tools/dev.sh` | Build, then flash through the Pi |
| `tools/flash.sh` | Copy images to the Pi and flash with esptool |
| `tools/restore-factory.sh` | Write the factory backup back from the Pi |

### Workflow

From this directory:

```sh
tools/dev.sh                   # build (log in build/build.log) and flash through the Pi
../../host/pi.sh ping          # test with the host client on the Pi
../../host/pi.sh capture /tmp/screen.png   # (on the Pi) save the rendered screen
```

- `tools/flash.sh` refuses to flash while another process holds the serial port (e.g. the logger in `~/stackchan-logs`)
- Host client: [host/](../../host/)

### Status

| Area | State |
|---|---|
| Link, JSON-RPC, errors, events, streams, logs | Implemented, tested |
| Power: `sys.status`, `button.get`, power events, `log.*` | Implemented, tested (`sys.power_off` not exercised) |
| Display: face, decorators, modal (ASCII + Hangul), brightness, power | Implemented, tested via `display.capture` |
| Touch, IMU / motion, light / proximity, RTC | Implemented, tested by reads |
| LEDs, audio, camera | Implemented, tested by reads and register checks |
| Servos: `servo.get` / `move` / `home` / `power`, `servo` stream, `servo.fault` | Implemented, tested with ±5° and ±20° moves and home (watched) |
| `servo.rotate` (wheel mode), spring motion | Not implemented |

### Physical Checklist

Checks that need a person at the device (not done yet):

| Item | How |
|---|---|
| LCD colors and orientation | Compare the screen with `display.capture` |
| Screen touch | Tap / long-press; `touch.screen` events, modal buttons |
| Head touch zones | Touch front / middle / back; confirm which zone name matches which spot |
| Power button, USB events | Short press → `button`; unplug / replug USB → `power.usb` (needs battery) |
| Motion | Tilt, lay down, shake, lift → `motion` events |
| Light / proximity | Lit room, cover the sensor → `lux`, `proximity` changes |
| Camera | Snapshot of a lit scene |
| Speaker | `audio.beep` is audible |
| LEDs | Colors, left / right sides, fade |
| Battery current sign | `batt_ma` on battery power (expected negative) |
| `sys.power_off` | Device turns off; power button turns it back on |

### Restore

`tools/restore-factory.sh` writes the full factory backup (verified by SHA256) to return to the official firmware.
