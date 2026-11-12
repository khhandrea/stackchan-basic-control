# StackChan Firmware Source

Source repository matching the firmware installed on the device (`ota_1`, active).

## Device Firmware

- Project: `stack-chan`, version 1.5.1
- ESP-IDF: v5.5.4
- Build date: 2026-07-31 10:38:34
- Same on device and in the factory backup ([factory-firmware.md](factory-firmware.md))

## Repository

| Item | Value |
|---|---|
| URL | https://github.com/m5stack/StackChan |
| Commit | `1b5765599fba8aaad1811d9a79358ccc7051f5f3` |
| Commit message | Merge pull request #112 from m5stack/firmware-dev — Update firmware v1.5.1 |
| Commit date | 2026-08-19 16:28:59 +0800 |
| Firmware dir | `firmware/` (`PROJECT_VER "1.5.1"`) |

- No tags or releases in the repository; versions are tracked by `PROJECT_VER` in `firmware/CMakeLists.txt`
- Later commits still at 1.5.1 (2026-08-30: `d2e9548`, `febcfdb`, `036eade`) are **not** in the device firmware
- Next version: 1.6.0 (`94856a7`, 2026-10-20)

### Match Evidence

Not verified by rebuilding (byte-for-byte); based on strings in the binary.

- 1.5.1-only strings present: `Apply ILI9342E panel initialization`, `FT6336 version read failed, panel:ILI9342C`
- Pre-2026-08-30 string present: `get wifi config: {} / {}` (removed in `febcfdb`)
- Post-2026-08-30 string absent: `setWifi is missing ssid or password` (added in `d2e9548`)
- 1.6.0 additions: almost none present (6/120, generic words only)

## Dependencies (`firmware/repos.json`)

Fetched by `firmware/fetch_repos.py` (not fetched in the local clone).

| Repository | Ref | Path |
|---|---|---|
| https://github.com/78/xiaozhi-esp32 | v2.2.4 + `patches/xiaozhi-esp32.patch` | `xiaozhi-esp32` |
| https://github.com/Forairaaaaa/mooncake | v2.3.3 | `components/mooncake` |
| https://github.com/Forairaaaaa/mooncake_log | v1.5.0 | `components/mooncake_log` |
| https://github.com/Forairaaaaa/smooth_ui_toolkit | v2.12.0 | `components/smooth_ui_toolkit` |
| https://github.com/bblanchon/ArduinoJson | v7.4.2 | `components/ArduinoJson` |
| https://github.com/espressif/esp-now | `c33383d` | `components/esp-now` |

## Power Save / Auto Power-off

| Item | Location |
|---|---|
| Board, PMIC (AXP2101), power save setup | `firmware/main/hal/board/stackchan.cc` (`M5StackCoreS3Board`, `Pmic`) |
| Settings load/save (NVS `xiaozhi`: `idle_sec`, `ext_pwr`) | `firmware/main/hal/board/hal_bridge.cc` |
| Settings UI (0–3600 s in 300 s steps) | `firmware/main/apps/app_setup/workers/ai_agent.cpp` |
| Timer logic | xiaozhi-esp32 `main/boards/common/power_save_timer.cc` |

- Timer enabled when discharging, or on external power with `ext_pwr = 1`
- Ticks every 1 s while `Application::CanEnterSleepMode()` is true (idle, no audio channel); otherwise reset to 0
- Sleep (display power save) after min(300 s, `idle_sec`), power-off via AXP2101 (reg 0x10 bit 0) after `idle_sec`
- `sleep_mode = false` in NVS namespace `wifi` disables the timer
