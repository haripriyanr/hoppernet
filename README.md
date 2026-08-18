# HopperNet (Spectrum-Pipe)

**100% Local, Cloudless, Jammer-Resilient FHSS Mesh with In-Memory SRAM Edge Buffering & SoftAP Web Consoles**

[![Microcontrollers](https://img.shields.io/badge/Platform-ESP32%20%7C%20Mega-blue.svg)]()
[![Radio](https://img.shields.io/badge/Transceiver-nRF24L01%2B-green.svg)](https://www.nordicsemi.com/)
[![Local Architecture](https://img.shields.io/badge/Network-100%25%20Local%20%2F%20No%20Cloud-orange.svg)]()
[![Displays](https://img.shields.io/badge/UI-SoftAP%20Web%20%7C%203.5%22%20Touch-purple.svg)]()

HopperNet is an embedded wireless mesh communication system built to maintain mission-critical data flow in RF-hostile and contested environments (e.g. disaster response, tactical fields, shielded structures).

Operating completely offline without internet or cloud dependencies, nodes communicate over the 2.4 GHz ISM band using nRF24L01+ transceivers driven by a slotted 50 ms Frequency Hopping Spread Spectrum (FHSS) protocol across 124 channels. When interference or an active RF jammer corrupts channels, the mesh detects energy, dynamically blacklists the channel in synchronized lockstep, and routes traffic cleanly. If destination nodes lose connection, relay nodes buffer packets in 520 KB SRAM memory, delivering them with 100% reliability once connectivity is restored.

---

## 1. System Topology

```
                  ┌──────────────────────────────────────────────┐
                  │    Local Desktop App / Node Web Portals      │
                  │    124-Channel Spectrum Heatmap & Telemetry  │
                  └──────────────────────┬───────────────────────┘
                                         │ Wi-Fi SoftAP / USB Serial
          ┌──────────────────────────────┼──────────────────────────────┐
          │                                                             │
   ┌──────▼──────┐               ┌──────────────┐               ┌───────▼──────┐
   │   Node A    │  SYNC/DATA/ACK│    Node B    │   DATA / ACK  │    Node C    │
   │ ESP32 DevKit│◄─────────────►│ ESP32 DevKit │──────────────►│ ESP32 DevKit │
   │ Source Node │               │ Master Relay │               │ Destination  │
   │ AP: hoppera │               │ AP: hopperb  │               │ AP: hopperc  │
   └─────────────┘               └───────┬──────┘               └──────────────┘
                                         ▲
                                         │ Active RF Jamming
                                 ┌───────┴──────┐
                                 │    Jammer    │
                                 │ Arduino Mega │
                                 │  Touchscreen │
                                 └──────────────┘
```

---

## 2. Hardware Bill of Materials & Interface Allocation

| Device | Microcontroller | Radio Module | UI / Network Interface | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Node A** | ESP32 DevKit | nRF24L01+ | Wi-Fi SoftAP (`hoppera`) / USB Serial | Source — Dispatches emergency messages, alerts & vitals |
| **Node B** | ESP32 DevKit | nRF24L01+ | Wi-Fi SoftAP (`hopperb`) / USB Serial | Master Relay — 50ms FHSS Master Clock, 520KB SRAM buffer, RPD jammer detector |
| **Node C** | ESP32 DevKit | nRF24L01+ | Wi-Fi SoftAP (`hopperc`) / USB Serial | Destination — Receives messages, sends delivery ACKs & return telemetry |
| **Jammer** | Arduino Mega 2560 | nRF24L01+ (PA/LNA) | 3.5" TFT Touchscreen / Serial CLI | RF Jammer — Multi-mode adversary console (Spot, Sweep, Random, Adaptive) |
| **Power** | 4× 10 µF Electrolytic Capacitors | — | — | Placed across VCC/GND at each radio header |

---

## 3. Quick Start & Flash Guide

```powershell
# Flash Node A (ESP32 Source)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_a
arduino-cli upload -p <PORT_A> --fqbn esp32:esp32:esp32 firmware/node_a

# Flash Node B (ESP32 Master Relay)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_b
arduino-cli upload -p <PORT_B> --fqbn esp32:esp32:esp32 firmware/node_b

# Flash Node C (ESP32 Destination)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_c
arduino-cli upload -p <PORT_C> --fqbn esp32:esp32:esp32 firmware/node_c

# Flash Jammer (Arduino Mega Adversary Console)
arduino-cli compile --fqbn arduino:avr:mega --library firmware/libraries/fhss firmware/jammer
arduino-cli upload -p <PORT_JAMMER> --fqbn arduino:avr:mega firmware/jammer
```

---

## 4. Live Demonstration Highlights

1. **Zero-Cloud Local SoftAP Portals**: Connect to `hoppera`, `hopperb`, or `hopperc` on any phone/laptop at `http://192.168.4.1` for real-time status and message dispatch.
2. **Touchscreen Cyber-Warfare Console**: Tap on the Jammer's 3.5" touchscreen to launch targeted spot or wideband sweep jamming.
3. **Zero-Loss Store-and-Forward**: Unplugging Node C causes packets to queue safely in Node B's SRAM custody buffer; reconnecting drains the queue with 100% delivery.
4. **Autonomous Dynamic Blacklisting**: Jammed channels are detected via RPD carrier energy in quiet tails and blacklisted dynamically across the mesh in lockstep.
