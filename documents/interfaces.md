# StackChan Interfaces

Buses, I2C addresses, GPIO map and expansion ports.

## Buses

### Internal I2C (SCL = G11, SDA = G12)

Core unit:

| Device | Part | Address |
|---|---|---|
| PMIC | AXP2101 | 0x34 |
| IO expander | AW9523B | 0x58 |
| RTC | BM8563 | 0x51 |
| IMU | BMI270 | 0x69 |
| Magnetometer | BMM150 | 0x10 (via BMI270 sensor hub) |
| Proximity / ambient light | LTR-553ALS-WA | 0x23 |
| Touch | FT6336U | 0x38 |
| Camera (SCCB) | GC0308 | 0x21 |
| Microphone ADC | ES7210 | 0x40 |
| Speaker amplifier | AW88298 | 0x36 |

Body:

| Device | Part | Address |
|---|---|---|
| Battery monitor | INA226 | 0x41 |
| NFC | ST25R3916 | 0x50 |
| Touch panel | Si12T | 0x68 |
| IO expander | PY32L020 | 0x6F (ADD_SEL = H: 0x71) |

- PY32L020 drives the RGB LEDs (WS2812C ×12) and servo power enable (VM_EN); the MCU does not drive them directly
  - Pin 0: VM_EN (servo power, output, high = on); pin 13: RGB data (output, push-pull)
  - LEDs: index 0–5 left, 6–11 right; colors as RGB565 in registers 0x30+ (2 bytes each, little-endian), count and refresh in 0x24
- INA226 measures the battery with a 10 mΩ shunt (StackChan-BSP): 2.5 µV/LSB shunt register → 0.25 mA/LSB
- Camera GC0308 has its own 20 MHz crystal: no XCLK pin; SCCB shares the internal I2C bus
- AW9523B: P0.2 = AW88298 reset, P1.1 = LCD reset (official firmware); FT6336 version selects the LCD init (ILI9342C/E)

### SPI (SCK = G36, MOSI = G37)

| Device | CS | Other |
|---|---|---|
| LCD (ILI9342C/E) | G3 | DC = G35 |
| microSD | G4 | MISO = G35 |

- G35 is shared between LCD DC and microSD MISO

### I2S (MCLK = G0, WS = G33, BCK = G34)

| Device | Data pin | Control |
|---|---|---|
| Microphone (ES7210) | DIN = G14 | Internal I2C |
| Speaker (AW88298) | DOUT = G13 | Internal I2C |

- Verified in the official firmware (`AUDIO_I2S_GPIO_DIN = 14`, `DOUT = 13`) and on the device
- I2S0: speaker in standard stereo mode, microphones in TDM mode (4 slots), 24 kHz, shared clocks

### Camera DVP (GC0308)

- D0–D7: G39, G40, G41, G42, G15, G16, G48, G47
- HREF = G38, PCLK = G45, VSYNC = G46
- Control via SCCB on internal I2C

### Servo Bus (UART1, TX = G6, RX = G7)

- Feetech SCS0009 ×2, half-duplex, 1 Mbps, addressed by servo ID

### Other

| Function | Interface |
|---|---|
| IR transmitter | G5 |
| IR receiver (IRM56384) | G10 |
| UART0 | TX = G43, RX = G44 |
| USB | USB-Serial/JTAG |

## GPIO Map

| GPIO | Function |
|---|---|
| G0 | I2S MCLK |
| G1 / G2 | PORT.A SCL / SDA (I2C) |
| G3 | LCD CS |
| G4 | microSD CS |
| G5 | IR transmitter |
| G6 / G7 | Servo TX / RX (UART1) |
| G8 / G9 | PORT.B (GPIO / ADC) |
| G10 | IR receiver |
| G11 / G12 | Internal I2C SCL / SDA |
| G13 | I2S DOUT (speaker) |
| G14 | I2S DIN (microphone) |
| G15, G16, G39–G42, G47, G48 | Camera D0–D7 |
| G17 / G18 | PORT.C TX / RX (UART) |
| G33 / G34 | I2S WS / BCK |
| G35 | LCD DC / microSD MISO (shared) |
| G36 / G37 | SPI SCK / MOSI |
| G38 / G45 / G46 | Camera HREF / PCLK / VSYNC |
| G43 / G44 | UART0 TX / RX |

- Reserved (stacked modules must not drive these): G5, G6, G7, G10, G11, G12

## Expansion Ports (HY2.0-4P)

| Port | 1 (black) | 2 (red) | 3 (yellow) | 4 (white) | Use |
|---|---|---|---|---|---|
| PORT.A | GND | 5V | G2 (SDA) | G1 (SCL) | I2C |
| PORT.B | GND | 5V | G9 | G8 | GPIO / ADC |
| PORT.C | GND | 5V | G17 (TX) | G18 (RX) | UART |

- Grove 5V output is software-controlled (`bus_5v`)
