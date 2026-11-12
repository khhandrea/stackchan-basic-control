# StackChan Device Report

Values measured on the actual unit. Nominal specifications are in [hardware.md](hardware.md).

- Date: 2026-10-26

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

## Firmware

- Factory partition table and backup: [factory-firmware.md](factory-firmware.md)
