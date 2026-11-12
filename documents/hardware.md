# StackChan Hardware

Component models and nominal specifications from official documentation.
Measured values from the actual device are in [report.md](report.md).

- Product: M5Stack StackChan (SKU K151 / K151-R with remote)

## MCU

- Chip: Espressif ESP32-S3 (QFN56)
- CPU: Xtensa LX7 dual-core, 240 MHz (+ LP core)
- Flash: 16 MB
- PSRAM: 8 MB, Quad SPI
- Wireless: Wi-Fi 2.4 GHz 802.11 b/g/n (**no 5 GHz**), Bluetooth LE 5
- USB: built-in USB-Serial/JTAG

## Core Unit (CoreS3-based)

| Function | Part |
|---|---|
| PMIC | X-Powers AXP2101 |
| IO expander | Awinic AW9523B |
| RTC | BM8563 |
| IMU (accel / gyro) | Bosch BMI270 |
| Magnetometer | Bosch BMM150 |
| Proximity / ambient light | Lite-On LTR-553ALS-WA |
| Touch controller | FocalTech FT6336U |
| Camera | GalaxyCore GC0308 (0.3 MP, 640×480) |
| Microphone ADC (2 ch) | Everest ES7210 |
| Speaker amplifier (1 W) | Awinic AW88298 |
| LCD controller | ILI9342C (ILI9342E on units from 2026-08) |

- LCD: 2.0" IPS, 320×240
  - ILI9342E boards require M5GFX ≥ 0.2.27
- microSD: up to 16 GB
- Buttons: power, reset (RST)

## Body

| Function | Part |
|---|---|
| Battery monitor | TI INA226 |
| NFC | ST ST25R3916 |
| Touch panel (3 zones) | Si12T (TSM12-compatible) |
| IO expander | Puya PY32L020 |
| IR receiver | IRM56384 |
| IR transmitter | - |
| RGB LED ×12 | WS2812C |

- PY32L020 handles servo power enable (VM_EN) and RGB LED driving
- INA226 battery current shunt: 10 mΩ
- Battery: 550 mAh, 3.7 V
- USB-C: use the body base port (power + data)
- Size / weight: 54.0 × 70.5 × 61.5 mm / 187.2 g
- LEGO-compatible mounting holes

## Servos

- Model: Feetech SCS0009 ×2 (serial bus servo with feedback)
- Axes (angles relative to the neutral pose, yaw 0 / pitch 0)
  - Yaw (X, ID 1, left/right)
    - Position mode: -128° to 128° (-128 = robot's left); official firmware limit ±128.0°
    - Wheel (PWM) mode: 360° continuous rotation, no position control
  - Pitch (Y, ID 2, up/down): 0–90° (90 = up), **recommended 5–85°** (outside this range → stall, risk of permanent damage); official firmware limit 3.0–87.0°
- Official firmware uses 0.1° internal units and raw position 0–1000 (zero offsets: yaw 460, pitch 620, stored in NVS `servo`)
- SCS0009 nominal ratings (vendor, at 6 V)
  - Voltage: 4–7.4 V
  - Stall torque: ~2.3 kg·cm / rated ~0.7 kg·cm
  - Speed: ~0.1 s/60° (no load)
  - Stall current: ~1.0 A
  - Position resolution: 0–1023 over ~300° (servo spec; the yaw wheel mode is what allows 360°)
  - Feedback: position, speed, voltage, temperature, load
  - Baud rate: 38400 bps – 1 Mbps
- Caution: do not turn the head by hand while powered on

See also: [interfaces.md](interfaces.md)
