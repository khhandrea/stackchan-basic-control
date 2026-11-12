# StackChan Serial Protocol

Message format between the host (Raspberry Pi 3) and the device over USB serial. It follows [JSON-RPC 2.0](https://www.jsonrpc.org/specification) with the `jsonrpc` field omitted.

## Framing

| Item | Value |
|---|---|
| Link | USB-Serial/JTAG (`/dev/ttyACM0` on the host) |
| Encoding | UTF-8 |
| Frame | One JSON object per line, terminated by `\n` |
| Max request line | 1024 bytes |
| Non-JSON lines | Lines not starting with `{` are raw console output (ROM / bootloader text, panics, driver messages printed from interrupts) and are not part of the protocol |
| Max response line | No fixed limit (a VGA `display.capture` is ~205 KB) |

## Message Types

| Type | Direction | Shape |
|---|---|---|
| Request | Host → Device | `{"id":…,"method":"…","params":{…}}` |
| Response | Device → Host | `{"id":…,"result":…}` or `{"id":…,"error":{…}}` |
| Notification | Device → Host | `{"method":"notify.…","params":{…}}` (no `id`) |

- `id` is a number or string chosen by the host and echoed in the response.
- `params` is optional when a method takes no parameters.
- A request without `id` is executed and gets no response.

```jsonc
{"id":7,"method":"sys.status"}
{"id":7,"result":{"uptime_ms":123456,"batt_pct":82,"batt_mv":4010,"batt_ma":-120,"charging":false,"vbus":true}}

{"id":8,"method":"servo.move","params":{"yaw":30,"pitch":95}}
{"id":8,"error":{"code":-32602,"message":"pitch out of range 5-85"}}
```

## Naming

| Item | Rule | Example |
|---|---|---|
| Methods | Lowercase `group.action`, snake_case words | `servo.move`, `led.set_all` |
| Fields | snake_case, unit suffix where a unit applies | `batt_mv`, `temp_c`, `uptime_ms` |
| Event names | Lowercase `group.kind` | `touch.head`, `power.usb` |

Units: angles in degrees, time in ms, voltage in mV, current in mA (negative = discharging), temperature in °C.

## Errors

| Code | Meaning |
|---|---|
| -32700 | Parse error (line is not valid JSON) |
| -32600 | Invalid request (missing `method`, line too long) |
| -32601 | Method not found |
| -32602 | Invalid params (missing, wrong type, out of range) |
| -32603 | Internal error |
| -32000 | Hardware error (I2C, servo bus) |
| -32001 | Not available (component failed to initialize) |
| -32002 | Servo power off or in fault state |
| -32003 | Busy (operation already in progress) |

## Notifications

### `notify.event`

Pushed when a subscribed event occurs.

| Field | Description |
|---|---|
| `seq` | Event sequence number, starts at 0 on boot, +1 per event sent; a gap means events were lost |
| `ts_ms` | Device uptime |
| `name` | Event name |
| other | Event-specific fields |

```jsonc
{"method":"notify.event","params":{"seq":42,"ts_ms":123500,"name":"touch.head","zone":"front","kind":"tap"}}
```

### `notify.stream`

Periodic values for a started stream.

| Field | Description |
|---|---|
| `seq` | Per-stream sequence number |
| `ts_ms` | Device uptime |
| `name` | Stream name |
| other | Same fields as the matching read method |

```jsonc
{"method":"notify.stream","params":{"seq":901,"ts_ms":124100,"name":"imu","ax":0.01,"ay":-0.02,"az":0.99}}
```

### `notify.log`

Human-readable debug text, including firmware logs and the periodic power log.

| Field | Description |
|---|---|
| `level` | `E`, `W`, `I`, `D`, `V` |
| `tag` | Source module |
| `msg` | Text |

```jsonc
{"method":"notify.log","params":{"level":"W","tag":"servo","msg":"pitch temp 61C"}}
```

## Events

`boot` and `error` are always sent. All other events are sent only when subscribed (`sys.subscribe`). Subscriptions reset on reboot.

| Name | Fields |
|---|---|
| `boot` | `fw_version`, `reset_reason`, `mac`, `free_heap` |
| `touch.head` | `zone` (`front` / `middle` / `back`), `kind` (`start` / `end` / `tap` / `long_press`) |
| `touch.screen` | `kind` (`tap` / `long_press`), `x`, `y` |
| `button` | `kind` (`press`) |
| `motion` | `kind` (`lifted` / `shaken` / `tilted` / `fallen`) |
| `proximity` | `state` (`near` / `far`), `value` |
| `power.usb` | `attached` (bool) |
| `power.charge` | `state` (`start` / `done` / `stop`) |
| `power.low_battery` | `batt_pct`, `batt_mv` |
| `servo.fault` | `servo` (`yaw` / `pitch`), `kind` (`overheat` / `overload` / `stall` / `bus_error`); servo power is cut |
| `modal.result` | `button` (`ok` / `yes` / `no`) or `timeout: true` |
| `error` | `kind` (`sensor_init` / `i2c` / `low_memory`), `detail` |

Subscription groups: `touch` (both touch events), `power` (all `power.*`), and each remaining event name.

Event details:

| Event | Behavior |
|---|---|
| `touch.screen` | `tap` when released before 600 ms, `long_press` once at 600 ms; coordinates are where the touch started. A tap on a visible modal goes to its buttons and produces no `touch.screen` event |
| `touch.head` | `start` / `end` per zone; `tap` if released within 500 ms; `long_press` once at 1 s |
| `motion` | Relative to the "up" direction measured at boot: `tilted` > 25° for 0.3 s, `fallen` > 60° for 1 s (re-armed when upright), `shaken` 4 jolts > 0.7 g within 0.8 s, `lifted` upward acceleration > 0.12 g for 60 ms while upright (heuristics) |
| `proximity` | `near` at raw ≥ 400, `far` below 250 (hysteresis) |
| `power.low_battery` | Once when on battery and `batt_pct` ≤ 10; re-armed at ≥ 15 % or on USB |
| `button` | AXP2101 power key short press (a long press of ~4 s powers the device off in hardware) |

## Methods

### System

| Method | Params | Result |
|---|---|---|
| `sys.ping` | — | `{"pong":true}` |
| `sys.info` | — | `fw_version`, `build_date`, `mac` |
| `sys.status` | — | `uptime_ms`, `batt_pct`, `batt_mv`, `batt_ma`, `charging`, `charge_state`, `vbus`, `vbus_mv`, `chip_temp_c`, `pmic_temp_c`, `free_heap` |
| `sys.subscribe` | `events`: list of names or groups | Current subscription list |
| `sys.unsubscribe` | `events`: list of names or groups | Current subscription list |
| `sys.reboot` | — | `{}` (sent before rebooting) |
| `sys.power_off` | — | `{}` (sent before shutting down) |
| `rpc.list` | — | List of method names |

### Streams and Log

| Method | Params | Result |
|---|---|---|
| `stream.start` | `name` (`status` / `imu` / `light` / `servo` / `audio`), `rate_hz` (1–100) | `{}`; `-32001` if the source is not available |
| `stream.stop` | `name` | `{}` |
| `log.set_level` | `level` (`E` / `W` / `I` / `D` / `V` / `none`) | `{}` |
| `log.set_power_interval` | `interval_s` (0 = off) | `{}` |

### Sensors

| Method | Params | Result |
|---|---|---|
| `touch.get` | — | `head` (`front`, `middle`, `back` booleans, `level`: 3 × 0–3), `screen` (`pressed`, `x`, `y`), `count`, `last_ms` |
| `button.get` | — | `pressed`, `count`, `last_ms` |
| `imu.get` | — | `ax`, `ay`, `az` (g), `gx`, `gy`, `gz` (°/s), `mx`, `my`, `mz` (µT, uncompensated, approximate), `tilt_deg`, `motion` (`still` / `moving` / `tilted` / `fallen`) |
| `light.get` | — | `lux`, `proximity` (raw 0–2047), `near` |
| `time.get` | — | `iso` (RTC time, `YYYY-MM-DDTHH:MM:SS`), `valid` (false after RTC power loss) |
| `time.set` | `iso` (2000–2099) | `{}`; also sets the system clock |

- `batt_ma`: INA226 battery current, `null` if the INA226 is missing; positive = into the battery (sign to be verified on battery power)
- RTC time zone: store UTC

### Display

| Method | Params | Result |
|---|---|---|
| `face.get` | — | Current expression, parameters, decorators, auto motion state |
| `face.set_expression` | `name` (`neutral` / `happy` / `angry` / `sad` / `doubt` / `sleepy`) | `{}` |
| `face.set_params` | Any of `eye_x`, `eye_y`, `eye_open`, `eye_rot`, `eye_size`, `mouth_open`, `mouth_rot` | `{}` |
| `face.set_decorator` | `name` (`heart` / `angry` / `sweat` / `dizzy` / `shy`), `on` | `{}` |
| `face.set_auto` | Any of `blink`, `breath` (bool) | `{}` |
| `modal.get` | — | Current modal or `null` |
| `modal.show` | `title`, `body` (at least one; UTF-8, ASCII and Hangul), `buttons` (`none` / `ok` / `yes_no`), `timeout_ms` (0 = persistent) | `{}`; replaces a visible modal without a result event |
| `modal.close` | — | `{}` (no result event) |
| `display.set_brightness` | `value` (0–255) | `{}` |
| `display.power` | `on` | `{}` |

### Servos

Servos stay as they are at boot; call `servo.power` with `on: true` before moving. Angles use the zero positions calibrated by the official firmware (NVS `servo`).

| Method | Params | Result |
|---|---|---|
| `servo.get` | — | Per servo (`yaw`, `pitch`): `angle` (`null` when unpowered), `target`, `temp_c`, `volt_mv`, `load` (−1023–1023), `torque`, `powered` |
| `servo.move` | `yaw` (-128–128 °, -128 = left) and/or `pitch` (5–85 °, 85 = up), optional `speed` (1–360 °/s, default 60) | `{}`; `-32002` if servo power is off |
| `servo.rotate` | `velocity` (-1000–1000) | **Not implemented**: `-32001` (wheel mode rewrites the servo's EEPROM angle limits) |
| `servo.home` | — | `{}`: yaw 0°, pitch 5° at 60 °/s |
| `servo.power` | `on` (true = VM_EN on, hold the present position; false = torque off and VM_EN off) | `{}` |

- `spring` motion parameters are not implemented; moves are linear at `speed`
- Protection (checked every 100 ms with torque on): temperature ≥ 65 °C → `overheat`, |load| ≥ 700 for 0.5 s → `overload`, > 5° from a reached target for 1 s → `stall`, no reply → `bus_error`; each cuts torque and servo power and sends `servo.fault`

### LEDs

Output per channel = `color` × `brightness` / 100, then clamped to 168 (safe limit from the official firmware). `led.get` returns the requested `color` values before scaling.

| Method | Params | Result |
|---|---|---|
| `led.get` | — | `colors`: 12 × `[r, g, b]` (0–5 left, 6–11 right), `brightness` |
| `led.set_all` | `color` (`[r, g, b]`) | `{}` |
| `led.set_side` | `side` (`left` / `right`), `color`, `fade_ms` | `{}` |
| `led.set_one` | `index` (0–11), `color` | `{}` |
| `led.set_brightness` | `value` (0–100 %, default 100) | `{}` |

### Audio

| Method | Params | Result |
|---|---|---|
| `audio.get` | — | `volume` (default 60), `level_rms` (microphone, 0–32767, smoothed), `level_dbfs` |
| `audio.set_volume` | `value` (0–100) | `{}` |
| `audio.beep` | `freq_hz` (20–12000), `duration_ms` (1–1000) | `{}` (queued, up to 4; `-32003` when full) |

### Camera

| Method | Params | Result |
|---|---|---|
| `camera.snapshot` | — | `width`, `height`, `jpeg` (base64, quality 80); takes 0.2–0.8 s |
| `camera.set_resolution` | `size` (`qqvga` / `qvga` / `vga`, default `qvga`) | `{}`; restarts the camera (~0.7 s) |

### Debug

Not part of the stable interface; for hardware bring-up.

| Method | Params | Result |
|---|---|---|
| `display.capture` | — | `width`, `height`, `format` (`rgb565be`), `data` (base64 of the next rendered frame) |
| `i2c.scan` | — | List of answering addresses on the internal bus |
| `i2c.read` | `addr`, `reg`, optional `len` (1–32) | `data`: list of bytes |
| `i2c.write` | `addr`, `reg`, `data` (1–32 bytes) | `{}` |
