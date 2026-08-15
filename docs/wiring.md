# Wiring — nRF24L01+ to each board

All three nodes use an **nRF24L01+** module (SPI). Note the module is **3.3V
logic** — never power it from 5V.

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

## Common notes

- Add a **10 µF electrolytic capacitor across VCC/GND** right at the nRF24L01+;
  the module has sharp current spikes that can brown-out the MCU.
- Keep module antenna area clear of ground planes / wires.
- If using a bare module, add a decoupling cap (0.1 µF) close to VCC.
