# Complete Hardware Pinout & Wiring Cheatsheet

---

## ⚡ Global Power & Safety Rules
1. **nRF24L01+ Power**: Connect **ONLY to 3.3V**. Connecting to 5V will destroy the radio module.
2. **Capacitor Rule (CRITICAL)**: Solder or bridge a **10 µF electrolytic (or 100 nF ceramic) capacitor** across `VCC` and `GND` directly at the nRF24 module pins to prevent voltage dropouts during RF transmission spikes.
3. **16×2 I2C LCD Power**: Connect `VCC` to **5V (VIN)** on the ESP32 (or 3.3V if your backpack supports 3.3V) and `GND` to ESP32 **GND**.

---

## 1. Node A — Source Endpoint (ESP32 DevKit)
**Role**: Dispatches emergency messages, telemetry, vitals, and image packets. Connected to PC via USB.

| nRF24L01+ Pin | ESP32 Pin | Wire Type | Description |
| :--- | :--- | :--- | :--- |
| **VCC (Pin 2)** | **3.3V** | Female–Female | Radio Power (3.3V ONLY) |
| **GND (Pin 1)** | **GND** | Female–Female | Common Ground |
| **CE (Pin 3)** | **GPIO 4** | Female–Female | Chip Enable / TX/RX mode |
| **CSN (Pin 4)** | **GPIO 5** | Female–Female | SPI Chip Select |
| **SCK (Pin 5)** | **GPIO 18** | Female–Female | Hardware VSPI Clock |
| **MOSI (Pin 6)** | **GPIO 23** | Female–Female | Hardware VSPI Master Out |
| **MISO (Pin 7)** | **GPIO 19** | Female–Female | Hardware VSPI Master In |
| **IRQ (Pin 8)** | *Unconnected* | — | Not used |

*Total Wires Needed for Node A*: **7× Female-to-Female**

---

## 2. Node B — Master Relay & Edge Buffer (ESP32 DevKit + 16×2 LCD)
**Role**: Master FHSS clock, dual-direction in-memory store-and-forward edge buffer (520KB RAM), 16×2 LCD display.

### A. Radio Wiring (nRF24L01+):
| nRF24L01+ Pin | ESP32 Pin | Wire Type | Description |
| :--- | :--- | :--- | :--- |
| **VCC (Pin 2)** | **3.3V** | Female–Female | Radio Power (3.3V ONLY) |
| **GND (Pin 1)** | **GND** | Female–Female | Ground |
| **CE (Pin 3)** | **GPIO 4** | Female–Female | Chip Enable |
| **CSN (Pin 4)** | **GPIO 5** | Female–Female | SPI Chip Select |
| **SCK (Pin 5)** | **GPIO 18** | Female–Female | VSPI Clock |
| **MOSI (Pin 6)** | **GPIO 23** | Female–Female | VSPI Master Out |
| **MISO (Pin 7)** | **GPIO 19** | Female–Female | VSPI Master In |

### B. 16×2 I2C LCD Wiring (I2C Backpack):
| 16×2 LCD Pin | ESP32 Pin | Wire Type | Description |
| :--- | :--- | :--- | :--- |
| **VCC** | **5V (VIN)** | Female–Female | LCD Logic & Backlight Power |
| **GND** | **GND** | Female–Female | Common Ground |
| **SDA** | **GPIO 21** | Female–Female | I2C Data |
| **SCL** | **GPIO 22** | Female–Female | I2C Clock |

*Total Wires Needed for Node B*: **11× Female-to-Female**

---

## 3. Node C — Destination Endpoint (ESP32 DevKit)
**Role**: Destination sink. Receives messages from Node A, sends ACKs and return messages back through Node B.

| nRF24L01+ Pin | ESP32 Pin | Wire Type | Description |
| :--- | :--- | :--- | :--- |
| **VCC (Pin 2)** | **3.3V** | Female–Female | Radio Power (3.3V ONLY) |
| **GND (Pin 1)** | **GND** | Female–Female | Ground |
| **CE (Pin 3)** | **GPIO 4** | Female–Female | Chip Enable |
| **CSN (Pin 4)** | **GPIO 5** | Female–Female | SPI Chip Select |
| **SCK (Pin 5)** | **GPIO 18** | Female–Female | VSPI Clock |
| **MOSI (Pin 6)** | **GPIO 23** | Female–Female | VSPI Master Out |
| **MISO (Pin 7)** | **GPIO 19** | Female–Female | VSPI Master In |
| **IRQ (Pin 8)** | *Unconnected* | — | Not used |

*Total Wires Needed for Node C*: **7× Female-to-Female**

---

## 4. Jammer — Adversary Console (Arduino Mega 2560 + 3.5" Touchscreen)
**Role**: Standalone RF carrier blaster with 3.5" TFT touchscreen interface & serial CLI.

### A. 3.5" Touchscreen Shield:
- **Plugs directly onto the Arduino Mega 2560 main headers** (covers pins D2–D13, A0–A5, 5V, GND). **Zero wires needed!**

### B. High-Power nRF24L01+ PA/LNA Radio Wiring (Back Header):
Connect to the 2-row header at the bottom/back edge of the Arduino Mega (pins 50–53):

| nRF24L01+ PA/LNA Pin | Arduino Mega Pin | Wire Type | Function |
| :--- | :--- | :--- | :--- |
| **VCC (Pin 2)** | **3.3V** | Male–Female | Radio Power (3.3V ONLY) |
| **GND (Pin 1)** | **GND** | Male–Female | Common Ground |
| **CE (Pin 3)** | **Pin 43** | Male–Female | Chip Enable (Open Header) |
| **CSN (Pin 4)** | **Pin 45** | Male–Female | SPI Chip Select (Open Header) |
| **SCK (Pin 5)** | **Pin 52** | Male–Female | Hardware SPI SCK |
| **MOSI (Pin 6)** | **Pin 51** | Male–Female | Hardware SPI MOSI |
| **MISO (Pin 7)** | **Pin 50** | Male–Female | Hardware SPI MISO |

*Total Wires Needed for Jammer*: **7× Male-to-Female**

---

## Total Jumper Wire Inventory Checklist

- **Female-to-Female (F-F)**: **25 wires**
  - Node A: 7
  - Node B: 11
  - Node C: 7
- **Male-to-Female (M-F)**: **7 wires**
  - Jammer: 7
