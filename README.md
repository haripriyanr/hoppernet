# MedRelay (HopperNet)

**Jammer-Resilient FHSS Wireless Mesh with Edge Buffering & Cloud Sync**

[![Microcontrollers](https://img.shields.io/badge/Platform-ESP32-blue.svg)](https://espressif.com/)
[![Radio](https://img.shields.io/badge/Transceiver-nRF24L01%2B-green.svg)](https://www.nordicsemi.com/)
[![Cloud](https://img.shields.io/badge/Backend-Supabase-emerald.svg)](https://supabase.com/)
[![Architecture](https://img.shields.io/badge/Topology-Store%20%26%20Forward%20Mesh-orange.svg)]()

MedRelay is an embedded wireless communication system built to maintain mission-critical data flow in RF-hostile and contested environments (e.g. hospitals, disaster zones, industrial plants). 

When interference or an active RF jammer corrupts specific frequency channels, the mesh detects the carrier energy, dynamically blacklists the channel in synchronized lockstep, and hops across 124 clean channels. If the destination node becomes unreachable, the relay node buffers packets in RAM and persistent flash (SPIFFS), delivering them with zero loss once connectivity is restored.

---

## 1. System Topology

```
                  ┌──────────────────────────────────────────────┐
                  │    MedRelay Live Dashboard & Supabase Cloud  │
                  │    124-Channel Spectrum Heatmap & Telemetry  │
                  └──────────────────────┬───────────────────────┘
                                         │ WiFi (2.4 GHz)
          ┌──────────────────────────────┼──────────────────────────────┐
          │                              │                              │
   ┌──────▼──────┐               ┌───────▼──────┐               ┌───────▼──────┐
   │   Node A    │  SYNC/DATA/ACK│    Node B    │   DATA / ACK  │    Node C    │
   │ ESP32 DevKit│◄─────────────►│ ESP32 DevKit │──────────────►│ ESP32 DevKit │
   │ Source Node │               │ Master Relay │               │ Destination  │
   └─────────────┘               └───────┬──────┘               └──────────────┘
                                         ▲
                                         │ Active RF Jamming
                                 ┌───────┴──────┐
                                 │    Jammer    │
                                 │ ESP32 DevKit │
                                 └──────────────┘
```

---

## 2. Key Features

1. **Deterministic FHSS Hopping Engine**: 124 RF channels (2.402–2.525 GHz) with a synchronized 25 ms dwell time driven by Node B's master microsecond clock.
2. **Dynamic Channel Blacklisting**: Real-time nRF24 Received Power Detector (RPD) scans detect jammer carriers during quiet slots and broadcast blacklist bitmaps in SYNC frames.
3. **Zero-Loss Persistent Edge Buffering**: Node B buffers undelivered packets across in-memory circular queues and SPIFFS flash memory, only clearing packets upon receiving positive downstream ACKs.
4. **Dual-Core FreeRTOS Multitasking**:
   - **Core 1**: Real-time microsecond-level SPI transactions and radio timing.
   - **Core 0**: Asynchronous WiFi management, HTTP REST polling, and telemetry streaming.
5. **Interactive Cloud Dashboard**: Real-time 124-channel spectrum grid, live node telemetry, edge buffer depth monitoring, and emergency alert dispatcher backed by Supabase WebSockets.

---

## 3. Hardware Bill of Materials

| Device | Microcontroller | Radio Module | Role |
| :--- | :--- | :--- | :--- |
| **Node A** | ESP32 DevKit (38-pin) | nRF24L01+ | Source — Dispatches user messages & vitals |
| **Node B** | ESP32 DevKit (38-pin) | nRF24L01+ | Master Relay — FHSS clock, SPIFFS buffer, jammer detector |
| **Node C** | ESP32 DevKit (38-pin) | nRF24L01+ | Destination — Receives messages, syncs to Supabase |
| **Jammer** | ESP32 DevKit (38-pin) | nRF24L01+ (PA/LNA) | RF Jammer — Multi-mode interference generator |
| **Power** | 4× 10 µF Electrolytic Capacitors | — | Placed across VCC/GND at each radio header |

### SPI Pinout (Nodes A, B, C)
- `VCC`: **3.3V** (Never connect to 5V!)
- `GND`: **GND**
- `CE`: **GPIO 4**
- `CSN`: **GPIO 5**
- `SCK`: **GPIO 18**
- `MOSI`: **GPIO 23**
- `MISO`: **GPIO 19**

*(Jammer uses `CE: GPIO 25` and `CSN: GPIO 26`)*

---

## 4. Quick Start & Flash Guide

### 1. Build & Upload Firmware
```powershell
# Flash Node A (Source)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_a
arduino-cli upload -p <PORT_A> --fqbn esp32:esp32:esp32 firmware/node_a

# Flash Node B (Relay & Master Clock)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_b
arduino-cli upload -p <PORT_B> --fqbn esp32:esp32:esp32 firmware/node_b

# Flash Node C (Destination)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_c
arduino-cli upload -p <PORT_C> --fqbn esp32:esp32:esp32 firmware/node_c

# Flash Jammer (Interference Generator)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/jammer
arduino-cli upload -p <PORT_JAMMER> --fqbn esp32:esp32:esp32 firmware/jammer
```

### 2. Launch Web Dashboard
Open [`dashboard/index.html`](file:///e:/repos/hoppernet/dashboard/index.html) in any modern browser (desktop or mobile). The dashboard immediately connects to the Supabase cloud backend to stream live spectrum data and node telemetry.

---

## 5. Live Demonstration Procedures

### Demo 1: Normal Mesh Operation & Message Dispatch
1. Open the dashboard on your phone.
2. Select a preset (e.g. **🚨 Code Blue: RM 302**) and click **DISPATCH**.
3. Observe the full journey: Phone ➔ Supabase ➔ Node A (WiFi) ➔ Node B (RF Hop) ➔ Node C (RF Hop) ➔ Supabase ➔ Delivered Feed on Dashboard.

### Demo 2: Store-and-Forward Buffer (Unplug Node C)
1. Unplug the USB cable from **Node C**.
2. Dispatch 3 messages from the dashboard.
3. Observe **Node B's buffer gauge** rising to `3 pkts` (held safely in flash).
4. Plug **Node C** back in.
5. Within seconds, Node B drains the entire buffer to Node C. All messages arrive with zero data loss.

### Demo 3: Active RF Jamming & Dynamic Blacklisting
1. Open the serial console for the Jammer ESP32 (115200 baud).
2. Type `c 52` then `j` to blast interference on Channel 52.
3. Observe **Node B**: Carrier scans detect the jammer and mark Channel 52 blacklisted.
4. Observe **Dashboard**: Channel 52 immediately lights up in **red** on the 124-channel spectrum heatmap.
5. The mesh continues hopping seamlessly on clean channels with zero packet drops.

---

## 6. Repository Layout

```
hoppernet/
├── firmware/
│   ├── libraries/
│   │   └── fhss/src/
│   │       ├── fhss.h             # Core FHSS protocol engine & PRNG
│   │       └── fhss_config.h      # WiFi, Supabase keys & pinouts
│   ├── node_a/node_a.ino          # ESP32 Source node
│   ├── node_b/node_b.ino          # ESP32 Master relay & SPIFFS edge buffer
│   ├── node_c/node_c.ino          # ESP32 Destination sink node
│   └── jammer/jammer.ino          # ESP32 Multi-mode RF jammer
├── dashboard/
│   ├── index.html                 # Live web dashboard interface
│   ├── style.css                  # Responsive dark-theme styling
│   └── app.js                     # Real-time Supabase Realtime client
├── docs/
│   ├── architecture.md            # System topology & layer breakdown
│   ├── protocol.md                # 25ms dwell slot & frame specifications
│   └── wiring.md                  # Detailed pinouts & power decoupling
├── tools/
│   └── serial_logger.py           # Multi-port serial capture utility
├── .env.example                   # Configuration template
├── .gitignore
├── AGENTS.md                      # Engineering runbook & developer guide
└── README.md                      # Project overview
```
