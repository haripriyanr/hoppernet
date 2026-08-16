# Hardware Wiring — nRF24L01+ to ESP32

All nodes in the HopperNet / MedRelay mesh use an **ESP32 DevKit** connected to an **nRF24L01+** 2.4 GHz transceiver module via SPI.

> [!WARNING]
> **Logic Level & Power**: The nRF24L01+ is **3.3V logic only**. Always connect `VCC` to the ESP32's **3.3V** pin, never 5V. Connecting to 5V will permanently destroy the radio chip.

---

## 1. Mesh Nodes A, B, and C (Identical Pinout)

Because Nodes A, B, and C are on separate ESP32 boards, they share the exact same hardware pinout:

| nRF24L01+ Pin | ESP32 Pin | Function / Description |
| :--- | :--- | :--- |
| **VCC** | **3.3V** | Module Power (3.3V DC rail) |
| **GND** | **GND** | Ground Common |
| **CE** | **GPIO 4** | Chip Enable (TX/RX mode switch) |
| **CSN** | **GPIO 5** | Chip Select Not (SPI bus select) |
| **SCK** | **GPIO 18** | SPI Serial Clock |
| **MOSI** | **GPIO 23** | SPI Master Out Slave In |
| **MISO** | **GPIO 19** | SPI Master In Slave Out |
| **IRQ** | *(Unused)* | Optional interrupt (polling mode used) |

---

## 2. Jammer / Adversary Node (ESP32 #4)

The Jammer runs on a dedicated 4th ESP32 board using alternative GPIO pins for CE/CSN to avoid any confusion:

| nRF24L01+ Pin | ESP32 Pin | Function / Description |
| :--- | :--- | :--- |
| **VCC** | **3.3V** | Module Power |
| **GND** | **GND** | Ground |
| **CE** | **GPIO 25** | Chip Enable |
| **CSN** | **GPIO 26** | Chip Select Not |
| **SCK** | **GPIO 18** | SPI Clock |
| **MOSI** | **GPIO 23** | SPI MOSI |
| **MISO** | **GPIO 19** | SPI MISO |

---

## 3. Power Stability & Decoupling Capacitor

> [!IMPORTANT]
> **Add a 10 µF Electrolytic Capacitor** directly across the `VCC` and `GND` pins of every nRF24L01+ module.
> - High-frequency RF transmissions generate sharp current transients (~15 mA on bare modules, up to 115 mA on PA/LNA modules) which cause voltage dips on the 3.3V rail.
> - Without this capacitor, the ESP32 or nRF24L01+ may reset intermittently or fail SPI initialization (`RF24 init FAILED`).

---

## 4. Hardware Inventory Summary

| # | Item | Purpose | Quantity |
|---|------|---------|:--------:|
| 1 | ESP32 Dev Board (Node A) | Source Node (Dispatches telemetry & alerts) | 1 |
| 2 | ESP32 Dev Board (Node B) | Relay + Edge Buffer + Master Clock | 1 |
| 3 | ESP32 Dev Board (Node C) | Destination Sink Node | 1 |
| 4 | ESP32 Dev Board (Jammer) | Active RF Interference Generator | 1 |
| 5 | nRF24L01+ Transceiver Modules | 2.4 GHz ISM Radios (PA/LNA preferred for jammer) | 4 |
| 6 | 10 µF Electrolytic Capacitors | Power line smoothing at radio header | 4 |
| 7 | Female-to-Female Jumper Wires | Interconnects | 28 |
