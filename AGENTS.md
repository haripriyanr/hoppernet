# AGENTS.md — HopperNet (MedRelay) Engineering Guide

## System Overview

**HopperNet / MedRelay** is an embedded **Jammer-Resilient FHSS Mesh with Persistent Edge Buffering and Cloud Synchronization**. 

Nodes communicate via 2.4 GHz nRF24L01+ transceivers using a slotted 25 ms Frequency Hopping Spread Spectrum (FHSS) protocol. When interference or an active RF jammer corrupts a frequency channel, the mesh detects the carrier, dynamically blacklists the channel in lockstep across all nodes, and buffers undelivered packets persistently in memory. All telemetry, channel states, and dispatched emergency messages synchronize in real-time with a Supabase cloud backend and a live web dashboard.

---

## Hardware Configuration

| Node | Microcontroller | Transceiver | Display / UI | Role | Pinout (CE / CSN) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Node A** | ESP32 DevKit | nRF24L01+ | WiFi + Cloud | Source — Dispatches user messages & vitals | GPIO 4 / GPIO 5 |
| **Node B** | Arduino Due (84MHz ARM) | nRF24L01+ | 16×2 I2C LCD | Master Relay — FHSS clock, 96KB buffer, jammer detector | Pin 9 / Pin 10 (SPI Header) |
| **Node C** | ESP32 DevKit | nRF24L01+ | WiFi + Cloud | Destination — Receives messages, syncs to Supabase | GPIO 4 / GPIO 5 |
| **Jammer** | Arduino Mega 2560 | nRF24L01+ (PA/LNA) | 3.5" Touchscreen | RF Jammer — Multi-mode interference adversary console | Pin 9 / Pin 53 (SPI 50-52) |

---

## Compilation & Upload Commands

### Node A (ESP32 Source)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_a
arduino-cli upload -p <PORT_A> --fqbn esp32:esp32:esp32 firmware/node_a
```

### Node B (Arduino Due Relay & Master Clock)
```powershell
arduino-cli compile --fqbn arduino:sam:arduino_due_x_dbg --library firmware/libraries/fhss firmware/node_b
arduino-cli upload -p <PORT_B> --fqbn arduino:sam:arduino_due_x_dbg firmware/node_b
```

### Node C (ESP32 Destination)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_c
arduino-cli upload -p <PORT_C> --fqbn esp32:esp32:esp32 firmware/node_c
```

### Jammer (Arduino Mega Adversary Console)
```powershell
arduino-cli compile --fqbn arduino:avr:mega --library firmware/libraries/fhss firmware/jammer
arduino-cli upload -p <PORT_JAMMER> --fqbn arduino:avr:mega firmware/jammer
```

---

## Testing & Competition Demo Runbook

### 1. Mesh Power-Up & Sync Verification
1. Power up **Node B** (Arduino Due). The 16×2 LCD lights up showing:
   ```
   CH:---  HOP:0
   BUF:0 pk JAM:0
   ```
2. Power up **Node A** and **Node C** (ESP32s). Within 2 seconds, both nodes capture the SYNC frame and output: `*** SYNC ACQUIRED ***`.
3. The 16×2 LCD on Node B begins hopping actively across channels.

### 2. Message Dispatch & Delivery Test
1. On your phone or laptop, open `dashboard/index.html`.
2. Click **🚨 Code Blue** and hit **DISPATCH**.
3. **Node A** pulls the message from Supabase over WiFi, embeds it into a FHSS DATA frame, and transmits it to Node B.
4. **Node B** receives, ACKs Node A, buffers, and forwards to Node C.
5. **Node C** receives, sends an ACK, and posts directly to Supabase over WiFi.
6. The message appears instantly on the dashboard feed marked `✓ DELIVERED`.

### 3. Edge Buffering & Store-and-Forward Demo (Zero-Loss Proof)
1. **Unplug Node C** (simulating doctor entering an RF dead-zone or elevator).
2. Dispatch 3 messages from the dashboard.
3. Observe **Node B's 16×2 LCD**:
   ```
   BUF: 3 pk JAM:0
   ```
   Packets are safely held in the Due's 96 KB SRAM buffer.
4. **Plug Node C back in**.
5. Within 500 ms, Node B detects Node C's ACKs and flushes all 3 buffered packets. The LCD drops back to `BUF: 0 pk`. Zero data loss!

### 4. RF Jammer Stress Test & Dynamic Blacklisting
1. On the **Arduino Mega Jammer**, use the touchscreen or type in Serial (115200 baud): `c 45` then `j` to start jamming Channel 45.
2. Observe **Node B's LCD**: `JAM: 1`. Carrier scans detect the jammer $\rightarrow$ `Blacklisted channel 45`.
3. Observe **Web Dashboard**: Channel 45 turns red in the 124-channel spectrum heatmap.
4. All three nodes automatically skip channel 45 in lockstep with zero packet loss.
5. Type `b` on the jammer to test the full 124-channel sweep resistance.
