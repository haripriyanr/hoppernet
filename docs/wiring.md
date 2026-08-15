# Wiring — nRF24L01+ to each board

All nodes use an **nRF24L01+** module (SPI). The jammer also uses an nRF24L01+
module. Note the module is **3.3V logic** — never power it from 5V.

## Node A — ESP32 (source)

| nRF24L01+ | ESP32 pin |
|-----------|-----------|
| VCC       | 3.3V      |
| GND       | GND       |
| CE        | GPIO4     |
| CSN       | GPIO5     |
| SCK       | GPIO18    |
| MOSI      | GPIO23    |
| MISO      | GPIO19    |
| IRQ       | (optional, unused) |

## Node B — Raspberry Pi 4 (relay)

| nRF24L01+ | RPi header pin |
|-----------|----------------|
| VCC       | 17 (3.3V)     |
| GND       | 25 (GND)      |
| CE        | 15 (GPIO22)   |
| CSN       | 24 (CE0)      |
| SCK       | 23 (SCLK)     |
| MOSI      | 19 (MOSI)     |
| MISO      | 21 (MISO)     |
| IRQ       | (optional)    |

Enable SPI first: `sudo raspi-config` → Interface Options → SPI → Yes.

## Node C — Arduino Due (destination)

| nRF24L01+ | Due pin |
|-----------|---------|
| VCC       | 3.3V    |
| GND       | GND     |
| CE        | D9      |
| CSN       | D10     |
| SCK       | D76 (ICSP SCLK) |
| MOSI      | D75 (ICSP MOSI) |
| MISO      | D74 (ICSP MISO) |
| IRQ       | (optional) |

## Jammer — ESP32 (RF jammer)

| nRF24L01+ | ESP32 pin |
|-----------|-----------|
| VCC       | 3.3V      |
| GND       | GND       |
| CE        | GPIO25    |
| CSN       | GPIO26    |
| SCK       | GPIO18    |
| MOSI      | GPIO23    |
| MISO      | GPIO19    |
| IRQ       | (optional, unused) |

Same SPI bus as Node A but **different CE/CSN**. Do NOT run Node A and Jammer
on the same ESP32 simultaneously — use separate boards.

## Common notes

- Add a **10 µF electrolytic capacitor across VCC/GND** right at the nRF24L01+;
  the module has sharp current spikes that can brown-out the MCU. This is
  **critical for the +PA/LNA antenna modules** (up to 115 mA TX current).
- Keep module antenna area clear of ground planes / wires.
- If using a bare module, add a decoupling cap (0.1 µF) close to VCC.

## Shopping list (4 modules)

| # | Module | For | Notes |
|---|--------|-----|-------|
| 1 | nRF24L01+ (bare or PA/LNA) | Node A (ESP32) | |
| 2 | nRF24L01+ (bare or PA/LNA) | Node B (Pi) | Bare recommended for Pi3.3V rail |
| 3 | nRF24L01+ (bare or PA/LNA) | Node C (Due) | |
| 4 | nRF24L01+ (PA/LNA preferred) | Jammer (ESP32) | Higher TX power = stronger jamming |
| — | 4× 10µF electrolytic caps | All nodes | Brown-out protection |
| — | Jumper wires (M-M or M-F) | All nodes | |
