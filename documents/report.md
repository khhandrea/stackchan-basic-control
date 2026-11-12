# StackChan Device Report

Values measured on the actual unit. Nominal specifications are in [hardware.md](hardware.md).

- Dates: 2026-10-26 (MCU, factory backup), 2026-11-13 (peripherals, with the general firmware)
- Conditions on 2026-11-13: powered from the Raspberry Pi USB port, battery full, room dark (night)

## MCU

| Item | Value |
|---|---|
| Chip | ESP32-S3 (QFN56), revision v0.2 |
| Crystal | 40 MHz |
| Flash | 16 MB, Quad SPI, 3.3 V |
| Flash ID | Manufacturer 0x46, Device 0x4018 |
| USB VID:PID | `303a:1001` (USB-Serial/JTAG) |
| MAC (Wi-Fi) | `80:45:6b:54:8a:e8` |
| PSRAM | 8 MB detected, 80 MHz (quad), from the general firmware boot log |
| Flash I/O | QIO works (`spi_flash: flash io: qio`) |

## Internal I2C Bus

Scan (`i2c.scan`): `0x21 0x23 0x34 0x36 0x38 0x40 0x41 0x50 0x51 0x58 0x68 0x69 0x6f` — all addresses in [interfaces.md](interfaces.md); BMM150 (0x10) is behind the BMI270 aux bus and not visible.

| Device | Address | ID / version read |
|---|---|---|
| GC0308 camera | 0x21 | PID 0x9B |
| LTR-553ALS | 0x23 | part ID 0x92, manufacturer 0x05 |
| AXP2101 | 0x34 | — |
| AW88298 | 0x36 | ID 0x1852 |
| FT6336 touch | 0x38 | cipher 0x64, firmware 0x12, vendor 0x11 |
| ES7210 | 0x40 | ID 0x72 (reg 0x3D) |
| INA226 | 0x41 | manufacturer `TI` (0x5449), die ID 0x2260 |
| ST25R3916 (NFC) | 0x50 | not used |
| BM8563 RTC | 0x51 | — |
| AW9523B | 0x58 | — |
| Si12T head touch | 0x68 | CTRL1 0x22, CTRL2 0x07 after setup |
| BMI270 | 0x69 | chip ID 0x24; BMM150 chip ID 0x32 via aux |
| PY32L020 IO expander | 0x6F | version 0x41, UID 0x5169 |

## Display

- Panel: **ILI9342E** (FT6336 firmware 0x12 / vendor 0x11 → units from 2026-08); the ILI9342E init sequence is required

## Power

| Item | Value |
|---|---|
| Battery | 4.12–4.14 V, 100 % (AXP2101 fuel gauge), charge state `done` |
| VBUS | 4.76–4.87 V (Raspberry Pi USB) |
| VSYS | 4.33 V |
| Battery current (INA226, 10 mΩ shunt) | +16 mA with charging done on USB; sign convention not yet verified on battery power |
| Charge current setting | 700 mA (AXP2101 reg 0x62 = 0x0D, set by the factory firmware) |
| AXP2101 die temperature | 46–50 °C |
| ESP32-S3 die temperature | 43–47 °C |

## Sensors (at rest, dark room)

| Sensor | Value |
|---|---|
| Accelerometer at rest | (0.01, 0.69, 0.71) g — the core unit sits tilted ~44° from the IMU z axis |
| Gyroscope at rest | ~0.1 °/s |
| Magnetometer (uncompensated) | (−195, −13, −34) µT — far above Earth's field; offset from nearby magnets (speaker) |
| Ambient light / proximity | 0 lux / 0 (also 0 with all LEDs at full white) |
| Camera (VGA, dark room) | uniform frame, mean 24/255, unchanged with the LEDs on |
| Microphone level (quiet room) | RMS ~20 (−64 dBFS) at 30 dB gain |
| RTC | running, valid; held UTC |

## Servos

| Item | Yaw (ID 1) | Pitch (ID 2) |
|---|---|---|
| Calibrated zero (NVS `servo`, official firmware) | raw 476 | raw 596 |
| Default zero (official firmware) | raw 460 | raw 620 |
| Pose found after the factory firmware | 17.2° | 43.8° |
| Supply voltage | 5.1 V | 5.2 V |
| Temperature (idle → after tests) | 26 → 28 °C | 27 → 29 °C |
| Steady-state error after moves | ≤ 1.6° (load ±75–90) | ≤ 1° |

- VM_EN (PY32 pin 0) was left on by the factory firmware
- With servo power off the servos do not answer on the bus

Light sensor and camera have to be re-checked in a lit room (see the firmware README checklist).

## Firmware

- Factory partition table and backup: [factory-firmware.md](factory-firmware.md)
