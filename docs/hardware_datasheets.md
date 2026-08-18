# HopperNet — Hardware Datasheets & Specifications

Complete component reference for every board, module, and display in the HopperNet (Spectrum-Pipe) mesh.

All datasheet PDFs and pinout diagrams are stored locally in [`docs/datasheets/`](datasheets/).

| # | Component | Used In | Datasheet (Local) | Pinout (Local) |
| :--- | :--- | :--- | :--- | :--- |
| 1 | ESP32 DevKit V1 (ESP32-WROOM-32) | Node A, Node B, Node C | [`ESP32-WROOM-32_Datasheet.pdf`](datasheets/ESP32-WROOM-32_Datasheet.pdf) | [`ESP32_DevKit_V1_Pinout.pdf`](datasheets/ESP32_DevKit_V1_Pinout.pdf) · [`..._Pinout.png`](datasheets/ESP32_DevKit_V1_Pinout.png) |
| 2 | nRF24L01+ (2.4 GHz transceiver) | Node A, Node B, Node C | [`nRF24L01+_Product_Spec_v1.0.pdf`](datasheets/nRF24L01+_Product_Spec_v1.0.pdf) | — |
| 3 | nRF24L01+ PA/LNA (high-power) | Jammer | [`nRF24L01+_PA_LNA_Module.pdf`](datasheets/nRF24L01+_PA_LNA_Module.pdf) | — |
| 4 | Arduino Mega 2560 Rev3 | Jammer | [`Arduino_Mega2560_Datasheet.pdf`](datasheets/Arduino_Mega2560_Datasheet.pdf) | [`Arduino_Mega2560_Pinout.pdf`](datasheets/Arduino_Mega2560_Pinout.pdf) |
| 5 | 3.5" TFT Touchscreen Shield (ILI9486 + XPT2046) | Jammer | [`3.5inch_TFT_Touch_Shield_Manual.pdf`](datasheets/3.5inch_TFT_Touch_Shield_Manual.pdf) · [`ILI9486_Controller_Datasheet.pdf`](datasheets/ILI9486_Controller_Datasheet.pdf) | See §5 below |

---

## 1. ESP32 DevKit V1 (ESP32-WROOM-32)

**Used in:** Node A (Source), Node B (Master Relay), Node C (Destination) — three identical boards.

### 1.1 Microcontroller Core (ESP32-D0WDQ6)

| Parameter | Specification |
| :--- | :--- |
| Architecture | Xtensa® LX6 dual-core 32-bit |
| Clock Speed | 160 / 240 MHz |
| ROM | 448 KB (boot ROM) |
| SRAM | 520 KB |
| RTC SRAM | 8 KB (16 KB total RTC memory) |
| External Flash | 4 MB SPI (ESP32-WROOM-32) |
| Operating Voltage | 3.0 – 3.6 V (nominal 3.3 V) |
| Power Modes | Active / Modem-Sleep / Light-Sleep / Deep-Sleep / Off |
| Recommended Operating Temp. | −40 °C to +85 °C |

### 1.2 Wireless (Wi-Fi & Bluetooth)

| Parameter | Specification |
| :--- | :--- |
| Wi-Fi | IEEE 802.11 b/g/n (2.4 GHz only, 20/40 MHz BW) |
| Wi-Fi Modes | STA, SoftAP, STA+AP |
| Wi-Fi TX Power | +20 dBm (802.11b, max) — firmware set to `WIFI_POWER_15dBm` |
| Wi-Fi RX Sensitivity | −97 dBm @ 1 Mbps DSSS (typ) |
| Bluetooth | BT 4.2 Classic + BLE, 0 dBm TX (typ), −97 dBm RX |
| Antenna | PCB antenna on module |

### 1.3 I/O & Peripherals

| Parameter | Specification |
| :--- | :--- |
| Digital I/O | 34 GPIO (30 exposed on DevKit V1 headers) |
| ADC | 2 × SAR ADC, up to 18 channels, 12-bit |
| DAC | 2 × 8-bit |
| UART | 3 (including UART0 = USB serial) |
| SPI | 3 (firmware uses VSPI on GPIO 18/19/23) |
| I²C | 3 (SCL/SDA) |
| PWM | LED PWM controller, 16 channels |
| Touch Sensing | 10 capacitive touch pads |
| Crypto HW | AES, SHA, RSA, RNG (ChaCha20 in this project is software) |

### 1.4 DevKit V1 Board Features

| Feature | Specification |
| :--- | :--- |
| Module | ESP32-WROOM-32 (2.4 GHz PCB antenna) |
| USB Bridge | CP2102 (USB-UART) |
| On-Board Regulator | 3.3 V LDO (AMS1117-class), ~500 mA–1 A |
| Input Voltage | 5 V via micro-USB or 5 V/VIN header |
| Headers | 2 × 19 pin (30 GPIO + power) |
| Buttons | EN (reset), BOOT (GPIO0) |
| LEDs | Power (red), GPIO2 (blue) |

> ⚠️ **Node use**: VSPI = SCK 18, MISO 19, MOSI 23; CE 4, CSN 5; radio **3.3 V ONLY** with a 10 µF decoupling cap across VCC/GND.

---

## 2. nRF24L01+ — 2.4 GHz Transceiver

**Used in:** Node A, Node B, Node C (standard modules).

| Parameter | Specification |
| :--- | :--- |
| Manufacturer | Nordic Semiconductor |
| Frequency Band | 2.400 – 2.525 GHz ISM (126 channels, 1 MHz spacing) |
| Modulation | GFSK |
| Air Data Rates | 250 kbps / 1 Mbps / 2 Mbps (**firmware: 2 Mbps**) |
| RF Channels Used | Channels 2–125 (124 hop channels) |
| TX Power Levels | −18 / −12 / −6 / 0 dBm |
| RX Sensitivity | −82 dBm @ 2 Mbps, −85 dBm @ 1 Mbps, −94 dBm @ 250 kbps |
| Supply Voltage | 1.9 – 3.6 V (**3.3 V only in this system**) |
| TX Current | 11.3 mA @ 0 dBm, 13.5 mA @ −6 dBm |
| RX Current | 12.3 mA (2 Mbps) |
| Standby/Power-Down | 26 µA / 900 nA |
| Packet Engine | Enhanced ShockBurst™ (auto-ACK, retransmit) — disabled for raw slotted mode |
| Receive Power Detector (RPD) | Hardware carrier/energy detector (used for jammer scanning) |
| Interface | 4-wire SPI, 8-pin DIP (CE, CSN, SCK, MOSI, MISO, IRQ, VCC, GND) |
| Typical Range | ~10–30 m indoor, ~100 m LOS |
| Operating Temp. | −40 °C to +85 °C |

> ⚠️ **Critical rule**: 3.3 V ONLY. 5 V destroys the module. Place a 10 µF cap across VCC/GND at the radio.

---

## 3. nRF24L01+ PA/LNA — High-Power Module

**Used in:** Jammer (adversary console) with external SMA antenna.

| Parameter | Specification |
| :--- | :--- |
| Base Chip | nRF24L01+ (identical radio core as §2) |
| Front-End | RFX2401C PA/LNA (2.4 GHz) |
| Max TX Power | **+20 dBm (100 mW)** via external PA |
| RX Gain | ~+20 dB LNA gain (effective RX sensitivity ≈ −102 dBm) |
| Antenna | External SMA (whip/panel), replaceable |
| Supply Voltage | 3.3 V (higher current draw than bare module) |
| TX Current (peak) | ~115 mA @ +20 dBm |
| Interface | Same 8-pin SPI (CE, CSN, SCK, MOSI, MISO, IRQ, VCC, GND) |
| Extra Components | On-board PA/LNA front-end, RF switch, matching network |

> ⚠️ **Capacitor rule is extra-critical**: PA/LNA current spikes exceed the ESP32/Mega regulator budget — a 10 µF (or larger) capacitor directly at VCC/GND is mandatory.
>
> ⚠️ **Jammer wiring**: CE = Mega Pin 43, CSN = Pin 45, SCK = Pin 52, MOSI = Pin 51, MISO = Pin 50.

---

## 4. Arduino Mega 2560 Rev3

**Used in:** Jammer (adversary console).

### 4.1 Microcontroller (ATmega2560)

| Parameter | Specification |
| :--- | :--- |
| Architecture | AVR 8-bit RISC |
| Clock Speed | 16 MHz |
| Flash | 256 KB (8 KB used by bootloader) |
| SRAM | 8 KB |
| EEPROM | 4 KB |
| Operating Voltage | 5 V |
| Input Voltage (recommended) | 7 – 12 V |
| Input Voltage (limits) | 6 – 20 V |
| DC Current per I/O Pin | 20 mA (40 mA absolute max) |
| DC Current (3.3 V pin) | 50 mA |
| Operating Temp. | −40 °C to +85 °C |

### 4.2 I/O & Peripherals

| Parameter | Specification |
| :--- | :--- |
| Digital I/O | 54 (15 PWM on pins 2–13, 44–46) |
| Analog Inputs | 16 × 10-bit ADC (A0–A15) |
| UART | 4 (Serial 0–3) |
| SPI | ICSP header: MISO 50, MOSI 51, SCK 52, SS 53 |
| I²C | SDA 20, SCL 21 |
| USB Bridge | ATmega16U2 (native USB) |
| Power | USB 5 V or DC barrel jack (7–12 V) |
| Board Dimensions | 101.52 × 53.3 mm |

### 4.3 Jammer Pin Assignments

| Function | Mega Pin | Notes |
| :--- | :--- | :--- |
| nRF24 CE | **43** | Rear header |
| nRF24 CSN | **45** | Rear header |
| nRF24 SCK | **52** | Hardware SPI |
| nRF24 MOSI | **51** | Hardware SPI |
| nRF24 MISO | **50** | Hardware SPI |
| TFT Shield | D2–D13, A0–A5 | Direct plug-on (stackable headers) |
| Touch XP | **9** | Resistive panel |
| Touch YM | **8** | Resistive panel |
| Touch YP | **A2** | Resistive panel (analog) |
| Touch XM | **A3** | Resistive panel (analog) |

---

## 5. 3.5" TFT Touchscreen Shield (ILI9486 + XPT2046)

**Used in:** Jammer — 3.5" touch UI for selecting jam mode, channel, dwell, and power.

### 5.1 Display (TFT LCD)

| Parameter | Specification |
| :--- | :--- |
| Screen Size | 3.5" diagonal |
| Resolution | **320 × 480 RGB** (portrait orientation) |
| LCD Controller | **ILI9486** (SPI) — some clones use ILI9488; `MCUFRIEND_kbv` auto-detects |
| Colors | 262K (18-bit / 24-bit RGB input, driven 16-bit 565) |
| Interface | SPI (16-bit parallel on shield bus via Mega) |
| Viewing Direction | 12 o'clock (varies by vendor) |
| Active Area | ~73.4 × 48.9 mm |

### 5.2 Touch Panel

| Parameter | Specification |
| :--- | :--- |
| Type | 4-wire resistive |
| Touch Controller | **XPT2046** (SPI) |
| Resolution | 4096 × 4096 raw (12-bit) → mapped to display coords |
| Calibration | Firmware `TS_MINX/TS_MINY/TS_MAXX/TS_MAXY` (see jammer.ino:24-27) |
| Pressure Threshold | `MINPRESSURE` 10, `MAXPRESSURE` 1000 (jammer.ino:29-30) |

### 5.3 Shield Electrical & Stacking

| Parameter | Specification |
| :--- | :--- |
| Compatible Hosts | Arduino UNO / Mega 2560 (Mega used here) |
| Mounting | Stackable headers, plugs directly onto D2–D13, A0–A5, 5V, GND |
| Backlight | On-shield backlight enable (default on) |
| Power | 5 V supply rail (LED backlight), logic 3.3/5 V tolerant via level resistors |
| Software Stack | `MCUFRIEND_kbv` (auto-detects ILI9486) + `Adafruit_GFX` + `TouchScreen` |

> ℹ️ **Note on drivers**: Vendors commonly mislabel 3.5" shields as ILI9481/ILI9488. `MCUFRIEND_kbv` reads the controller ID at boot; the Jammer build (jammer.ino) is confirmed working on ILI9486.

---

## 6. Power & Decoupling Reference

| Item | Spec | Notes |
| :--- | :--- | :--- |
| ESP32 DevKit Supply | 5 V USB / VIN → 3.3 V LDO | Radio draws from 3.3 V rail |
| nRF24L01+ (all nodes) | 3.3 V ±0.3 V | **Never 5 V** |
| nRF24L01+ PA/LNA (jammer) | 3.3 V, ≥ 115 mA peak | 10 µF cap mandatory |
| 10 µF Electrolytic Caps | 4 total | One at each radio VCC/GND header |
| Mega 2560 Supply | 7–12 V DC barrel or USB 5 V | On-board 5 V + 3.3 V regulators |

---

## 7. Firmware Mapping Reference

| Firmware Target | Board | Radio | Display | Key Config File |
| :--- | :--- | :--- | :--- | :--- |
| `firmware/node_a` | ESP32 DevKit V1 | nRF24L01+ | SoftAP Web `hoppera` | `fhss_config.h` |
| `firmware/node_b` | ESP32 DevKit V1 | nRF24L01+ | SoftAP Web `hopperb` | `fhss_config.h` |
| `firmware/node_c` | ESP32 DevKit V1 | nRF24L01+ | SoftAP Web `hopperc` | `fhss_config.h` |
| `firmware/jammer` | Arduino Mega 2560 | nRF24L01+ PA/LNA | 3.5" TFT Touch Shield | `jammer.ino` |

Common electrical rules that apply everywhere: **3.3 V radio rail**, **10 µF at each radio**, CE/CSN as documented in [wiring.md](wiring.md).
