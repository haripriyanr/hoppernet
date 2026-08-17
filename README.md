# MedRelay (HopperNet)

**Jammer-Resilient FHSS Wireless Mesh with Edge Buffering, On-Device Displays & Cloud Sync**

[![Microcontrollers](https://img.shields.io/badge/Platform-ESP32%20%7C%20Due%20%7C%20Mega-blue.svg)]()
[![Radio](https://img.shields.io/badge/Transceiver-nRF24L01%2B-green.svg)](https://www.nordicsemi.com/)
[![Cloud](https://img.shields.io/badge/Backend-Supabase-emerald.svg)](https://supabase.com/)
[![Displays](https://img.shields.io/badge/Displays-16x2%20LCD%20%7C%203.5%22%20Touch-purple.svg)]()

MedRelay is an embedded wireless communication system built to maintain mission-critical data flow in RF-hostile and contested environments (e.g. hospitals, disaster zones, industrial plants). 

When interference or an active RF jammer corrupts specific frequency channels, the mesh detects the carrier energy, dynamically blacklists the channel in synchronized lockstep, and hops across 124 clean channels. If the destination node becomes unreachable, the relay node buffers packets in its 96 KB SRAM memory, delivering them with zero loss once connectivity is restored.

---

## 1. System Topology

```
                  ┌──────────────────────────────────────────────┐
                  │    MedRelay Live Dashboard & Supabase Cloud  │
                  │    124-Channel Spectrum Heatmap & Telemetry  │
                  └──────────────────────┬───────────────────────┘
                                         │ WiFi (2.4 GHz)
          ┌──────────────────────────────┼──────────────────────────────┐
          │                                                             │
   ┌──────▼──────┐               ┌──────────────┐               ┌───────▼──────┐
   │   Node A    │  SYNC/DATA/ACK│    Node B    │   DATA / ACK  │    Node C    │
   │ ESP32 DevKit│◄─────────────►│ Arduino Due  │──────────────►│ ESP32 DevKit │
   │ Source Node │               │ Relay & LCD  │               │ Destination  │
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

## 2. Hardware Bill of Materials & Display Allocation

| Device | Microcontroller | Radio Module | Display / UI | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Node A** | ESP32 DevKit | nRF24L01+ | WiFi + Cloud | Source — Dispatches user messages & vitals |
| **Node B** | Arduino Due (84MHz ARM) | nRF24L01+ | 16×2 I2C LCD | Master Relay — FHSS clock, 96KB buffer, jammer detector |
| **Node C** | ESP32 DevKit | nRF24L01+ | WiFi + Cloud | Destination — Receives messages, syncs to Supabase |
| **Jammer** | Arduino Mega 2560 | nRF24L01+ (PA/LNA) | 3.5" Touchscreen | RF Jammer — Multi-mode adversary console |
| **Power** | 4× 10 µF Electrolytic Capacitors | — | — | Placed across VCC/GND at each radio header |

---

## 3. Quick Start & Flash Guide

```powershell
# Flash Node A (ESP32 Source)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_a
arduino-cli upload -p <PORT_A> --fqbn esp32:esp32:esp32 firmware/node_a

# Flash Node B (Arduino Due Relay + 16x2 LCD)
arduino-cli compile --fqbn arduino:sam:arduino_due_x_dbg --library firmware/libraries/fhss firmware/node_b
arduino-cli upload -p <PORT_B> --fqbn arduino:sam:arduino_due_x_dbg firmware/node_b

# Flash Node C (ESP32 Destination)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_c
arduino-cli upload -p <PORT_C> --fqbn esp32:esp32:esp32 firmware/node_c

# Flash Jammer (Arduino Mega Adversary Console)
arduino-cli compile --fqbn arduino:avr:mega --library firmware/libraries/fhss firmware/jammer
arduino-cli upload -p <PORT_JAMMER> --fqbn arduino:avr:mega firmware/jammer
```

---

## 4. Live Demonstration Highlights

1. **Physical On-Device LCD Proof**: Node B's 16×2 LCD displays real-time hopping channel, hop counter, store-and-forward buffer depth (`BUF: 3 pk`), and blacklisted channel counts (`JAM: 1`).
2. **Touchscreen Cyber-Warfare Console**: Judges can tap on the Jammer's 3.5" touchscreen to launch targeted or wideband sweeps.
3. **Zero-Loss Store-and-Forward**: Unplugging Node C causes packets to queue in Node B's 96 KB SRAM buffer; reconnecting drains the queue with 100% packet delivery ratio.
4. **Cloud & Phone Integration**: Real-time 124-channel spectrum heatmap and message dispatch from any phone browser via Supabase WebSockets.
