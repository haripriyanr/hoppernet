# Hardware Wiring Guide — HopperNet (MedRelay)

Complete wiring specifications for the 4-node competition setup:
- **Node A (Source)**: ESP32 DevKit
- **Node B (Relay & Edge Buffer)**: Arduino Due + 16×2 I2C LCD
- **Node C (Destination)**: ESP32 DevKit
- **Jammer (Adversary Console)**: Arduino Mega 2560 + 3.5" Touchscreen

---

## 1. nRF24L01+ 8-Pin Header Reference

Looking directly at the **2×4 pin header** on the back of the nRF24 module (with the antenna facing **UP**):

```
          [ ANTENNA ]
     ┌───────────────────┐
     │  (1) GND │ (2) VCC│  <-- 3.3V ONLY (Never 5V!)
     │  (3) CE  │ (4) CSN│
     │  (5) SCK │ (6) MOSI
     │  (7) MISO│ (8) IRQ│
     └───────────────────┘
```

> [!CAUTION]
> **Power Rule**: Connect `VCC` **ONLY to 3.3V**. Connecting to 5V will destroy the radio module.
>
> **Capacitor Rule**: Solder or insert a **10 µF electrolytic capacitor** directly between `VCC (+)` and `GND (-)` on each nRF24 module.

---

## 2. Node A (ESP32 — Source)

| nRF24L01+ Pin | ESP32 Pin | Wire Type | Purpose |
| :--- | :--- | :---: | :--- |
| **VCC** | **3.3V** | F-F | Power (3.3V DC) |
| **GND** | **GND** | F-F | Ground |
| **CE** | **GPIO 4** | F-F | Chip Enable |
| **CSN** | **GPIO 5** | F-F | SPI Chip Select |
| **SCK** | **GPIO 18** | F-F | SPI Clock |
| **MOSI** | **GPIO 23** | F-F | SPI Data In |
| **MISO** | **GPIO 19** | F-F | SPI Data Out |

*Total Wires for Node A: 7× Female-to-Female (F-F)*

---

## 3. Node B (Arduino Due — Master Relay & 16×2 LCD)

### A. nRF24L01+ $\rightarrow$ Arduino Due (Central SPI Header)
*Note: Hardware SPI on the Due is on the **central 6-pin header**.*

```
                 [ USB Ports ]
                       ▲
                ┌─────────────┐
 (MISO) Pin 1   │  ●       ●  │ Pin 2 (5V - DO NOT USE!)
  (SCK) Pin 3   │  ●       ●  │ Pin 4 (MOSI)
(Reset) Pin 5   │  ●       ●  │ Pin 6 (GND)
                └─────────────┘
```

| nRF24L01+ Pin | Arduino Due Pin | Wire Type | Purpose |
| :--- | :--- | :---: | :--- |
| **VCC** | **3.3V (Power Header)** | M-F | 3.3V Power |
| **GND** | **GND (Power Header)** | M-F | Ground |
| **CE** | **Pin 9 (D9)** | M-F | Chip Enable |
| **CSN** | **Pin 10 (D10)** | M-F | SPI Chip Select |
| **SCK** | **SPI Header Pin 3** | F-F | SPI Clock (center header middle-left) |
| **MOSI** | **SPI Header Pin 4** | F-F | SPI MOSI (center header middle-right) |
| **MISO** | **SPI Header Pin 1** | F-F | SPI MISO (center header top-left) |

### B. 16×2 I2C LCD $\rightarrow$ Arduino Due
| 16×2 LCD Pin | Arduino Due Pin | Wire Type |
| :--- | :--- | :---: |
| **VCC** | **5V (Power Header)** | M-F |
| **GND** | **GND (Power Header)** | M-F |
| **SDA** | **Pin 20 (SDA)** | M-F |
| **SCL** | **Pin 21 (SCL)** | M-F |

*Total Wires for Node B: 3× Female-to-Female (F-F) + 8× Male-to-Female (M-F)*

---

## 4. Node C (ESP32 — Destination Sink)

| nRF24L01+ Pin | ESP32 Pin | Wire Type | Purpose |
| :--- | :--- | :---: | :--- |
| **VCC** | **3.3V** | F-F | Power (3.3V DC) |
| **GND** | **GND** | F-F | Ground |
| **CE** | **GPIO 4** | F-F | Chip Enable |
| **CSN** | **GPIO 5** | F-F | SPI Chip Select |
| **SCK** | **GPIO 18** | F-F | SPI Clock |
| **MOSI** | **GPIO 23** | F-F | SPI Data In |
| **MISO** | **GPIO 19** | F-F | SPI Data Out |

*Total Wires for Node C: 7× Female-to-Female (F-F)*

---

## 5. Jammer (Arduino Mega 2560 — RF Adversary & 3.5" Touchscreen)

### A. nRF24L01+ $\rightarrow$ Arduino Mega 2560
| nRF24L01+ Pin | Arduino Mega Pin | Wire Type | Purpose |
| :--- | :--- | :---: | :--- |
| **VCC** | **3.3V (Power Header)** | M-F | 3.3V Power |
| **GND** | **GND (Power Header)** | M-F | Ground |
| **CE** | **Pin 9** | M-F | Chip Enable |
| **CSN** | **Pin 53** | M-F | SPI Chip Select (SS) |
| **SCK** | **Pin 52** | M-F | SPI Clock |
| **MOSI** | **Pin 51** | M-F | SPI MOSI |
| **MISO** | **Pin 50** | M-F | SPI MISO |

### B. 3.5" TFT Touchscreen Shield
- Plugs directly on top of the Mega's main header (Pins D2–D13, A0–A5).
- Pins 50–53 on the rear double row remain open and accessible for the nRF24L01+ wires.

*Total Wires for Jammer: 7× Male-to-Female (M-F)*

---

## 6. Master Shopping & Jumper Wire Checklist

| Connection Group | Female-to-Female (F-F) | Male-to-Female (M-F) |
| :--- | :---: | :---: |
| **Node A (ESP32)** | 7 | 0 |
| **Node B (Due + 16×2 LCD)** | 3 | 8 |
| **Node C (ESP32)** | 7 | 0 |
| **Jammer (Mega 2560)** | 0 | 7 |
| **TOTAL TO GRAB** | **17 wires** | **15 wires** |
