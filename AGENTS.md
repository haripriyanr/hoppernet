# AGENTS.md — HopperNet (MedRelay) Engineering Guide

## System Overview

**HopperNet / MedRelay** is an embedded **Jammer-Resilient FHSS Mesh with Persistent Edge Buffering and Cloud Synchronization**. 

Nodes communicate via 2.4 GHz nRF24L01+ transceivers using a slotted 25 ms Frequency Hopping Spread Spectrum (FHSS) protocol. When interference or an active RF jammer corrupts a frequency channel, the mesh detects the carrier, dynamically blacklists the channel in lockstep across all nodes, and buffers undelivered packets persistently in SPIFFS flash memory. All telemetry, channel states, and dispatched emergency messages synchronize in real-time with a Supabase cloud backend and a live web dashboard.

---

## Hardware Configuration (All ESP32)

| Node | Microcontroller | Transceiver | Role | Pinout (CE / CSN) |
| :--- | :--- | :--- | :--- | :--- |
| **Node A** | ESP32 DevKit | nRF24L01+ | Source — Dispatches user messages & vitals | GPIO 4 / GPIO 5 |
| **Node B** | ESP32 DevKit | nRF24L01+ | Master Relay — FHSS clock, SPIFFS buffer, jammer detector | GPIO 4 / GPIO 5 |
| **Node C** | ESP32 DevKit | nRF24L01+ | Destination — Receives messages, syncs to Supabase | GPIO 4 / GPIO 5 |
| **Jammer** | ESP32 DevKit | nRF24L01+ | RF Jammer — Multi-mode interference adversary | GPIO 25 / GPIO 26 |

*Common SPI bus on ESP32: SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23. All modules require a 10 µF capacitor across VCC/GND.*

---

## Compilation & Upload Commands

### Node A (Source)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_a
arduino-cli upload -p COM8 --fqbn esp32:esp32:esp32 firmware/node_a
```

### Node B (Relay & Master Clock)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_b
arduino-cli upload -p <NODE_B_PORT> --fqbn esp32:esp32:esp32 firmware/node_b
```

### Node C (Destination)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_c
arduino-cli upload -p COM9 --fqbn esp32:esp32:esp32 firmware/node_c
```

### Jammer (Adversary)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/jammer
arduino-cli upload -p <JAMMER_PORT> --fqbn esp32:esp32:esp32 firmware/jammer
```

---

## Testing & Competition Demo Runbook

### 1. Mesh Power-Up & Sync Verification
1. Power up **Node B** (Master Clock). Observe serial output: `[NODE_B] Mesh ready. Starting master FHSS clock...`
2. Power up **Node A** and **Node C**. Within 2 seconds, both nodes capture the SYNC frame and output: `*** SYNC ACQUIRED ***`.
3. Open `dashboard/index.html` on your phone or laptop. The top status displays **Cloud Sync Active**, with live channel highlights hopping across the 124-channel grid.

### 2. Message Dispatch & Delivery Test
1. On the web dashboard, click **🚨 Code Blue** and hit **DISPATCH**.
2. **Node A** pulls the message from Supabase over WiFi, embeds it into a FHSS DATA frame, and transmits it to Node B.
3. **Node B** receives, ACKs Node A, buffers, and forwards to Node C.
4. **Node C** receives, sends an ACK, and posts directly to Supabase.
5. The message appears instantly on the dashboard feed marked `✓ DELIVERED`.

### 3. Edge Buffering & Store-and-Forward Demo (Zero-Loss Proof)
1. **Unplug Node C** (simulating doctor entering an RF dead-zone or elevator).
2. Dispatch 3 messages from the dashboard.
3. Observe **Node B's serial & dashboard gauge**: `Store & Fwd Buffer: 3 pkts`. Packets are safely held in RAM and SPIFFS flash.
4. **Plug Node C back in**.
5. Within 500 ms, Node B detects Node C's ACKs and flushes all 3 buffered packets in order. The buffer gauge drops back to `0`. Zero data loss!

### 4. RF Jammer Stress Test & Dynamic Blacklisting
1. Open serial terminal to the **Jammer ESP32** (115200 baud).
2. Type `c 45` then `j` to start continuous jamming on Channel 45 (or `r` for random jamming).
3. Observe **Node B**: Carrier scans detect the jammer $\rightarrow$ `Blacklisted channel 45`.
4. Observe **Web Dashboard**: Channel 45 turns red in the 124-channel spectrum heatmap.
5. All three nodes automatically skip channel 45 in lockstep with zero packet loss.
6. Type `b` on the jammer to test the full 124-channel sweep resistance.
