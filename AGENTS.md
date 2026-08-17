# AGENTS.md — HopperNet (Spectrum-Pipe) Engineering Guide

## System Overview

**HopperNet (Spectrum-Pipe)** is a **100% Local, Cloudless, Jammer-Resilient FHSS Mesh with In-Memory SRAM Edge Buffering and Local Web Access Points**.

All communication operates independently of any internet or external cloud infrastructure. Nodes communicate over the 2.4 GHz band using nRF24L01+ transceivers driven by a slotted 25 ms Frequency Hopping Spread Spectrum (FHSS) protocol.

---

## Current Hardware & Network Architecture

| Node | Microcontroller | Transceiver | UI & Network Interface | Role | Pinout (CE / CSN) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Node A** | ESP32 DevKit | nRF24L01+ | Wi-Fi SoftAP: **`hoppera`** (`192.168.4.1`) / USB Serial | Source Endpoint — Dispatches alerts, vitals & data to Node C | GPIO 4 / GPIO 5 |
| **Node B** | ESP32 DevKit | nRF24L01+ | Wi-Fi SoftAP: **`hopperb`** (`192.168.4.1`) / USB Serial | Master Relay — 25ms Master FHSS Clock, 520KB SRAM dual-direction buffer, RPD Jammer detector | GPIO 4 / GPIO 5 |
| **Node C** | ESP32 DevKit | nRF24L01+ | Wi-Fi SoftAP: **`hopperc`** (`192.168.4.1`) / USB Serial | Destination Endpoint — Receives messages, sends ACKs and return telemetry | GPIO 4 / GPIO 5 |
| **Jammer** | Arduino Mega 2560 | nRF24L01+ (PA/LNA) | 3.5" TFT Touchscreen Shield / Serial CLI | Adversary Console — Spot, Sweep (124 ch), Random & Adaptive jamming modes | Pin 43 / Pin 45 (SPI 50–52) |

> **Wi-Fi Credentials for All Nodes**:
> - Password: `password`
> - Default Gateway: `http://192.168.4.1`

---

## How Current Device Communication Works (Protocol & Data Flow)

### 1. Zero Cloud / 100% Local Operation
- **No Internet Required**: Nodes run completely offline. No WiFi router or phone hotspot is needed.
- **Each ESP32 hosts its own Wi-Fi Access Point**:
  - Connect your phone/laptop to **`hoppera`** $\rightarrow$ open `http://192.168.4.1` to transmit forward messages.
  - Connect to **`hopperb`** $\rightarrow$ open `http://192.168.4.1` to view relay buffer depth (`fwd_buf`, `rev_buf`, `jam_count`).
  - Connect to **`hopperc`** $\rightarrow$ open `http://192.168.4.1` to view received messages and transmit reverse replies.
- **Local Desktop App (`app/main.py`)**:
  - Connects to all plugged USB COM ports simultaneously.
  - Auto-detects device roles (`NODE_A`, `NODE_B`, `NODE_C`, `JAMMER`).
  - Provides a unified web interface on `http://localhost:8000`.

---

### 2. 25-Millisecond Slotted FHSS Protocol Flow

Every 25 ms dwell interval hops across the 124 available frequencies ($2.402\text{ GHz to }2.525\text{ GHz}$) divided into strict microsecond time slots:

```
0 ms              2 ms                     13 ms                    24 ms         25 ms
├── SYNC Phase ───┼── FORWARD: A ➔ B ➔ C ──┼── REVERSE: C ➔ B ➔ A ──┼─ CARRIER ───┤
│ Node B Sync     │ A sends to B (2-7ms)   │ C sends to B (13-18ms) │ RPD Energy  │
│ Beacon (124 ch) │ B drains to C (7-13ms) │ B drains to A (18-24ms)│ Jammer Scan │
```

#### Phase Breakdown:
1. **Phase 1: SYNC Beacon ($0\text{ ms} \rightarrow 2\text{ ms}$)**:
   - **Node B** (Master Clock) transmits a `FRAME_TYPE_SYNC` packet containing:
     - Current Hop Index (32-bit)
     - Master Microsecond Timestamp (32-bit)
     - 16-byte Active Dynamic Blacklist Mask
   - **Node A & Node C** listen. When captured, they compute `clock_offset = master_ts - micros()` and hop in exact mathematical lockstep.
2. **Phase 2: Forward Path A $\rightarrow$ B ($2\text{ ms} \rightarrow 7\text{ ms}$)**:
   - **Node A** transmits its queued outbound data frame (`FRAME_TYPE_DATA`) to Node B.
   - **Node B** receives, saves into its `fwd_queue` in SRAM, and sends an immediate ACK frame back to Node A.
3. **Phase 3: Forward Drain B $\rightarrow$ C ($7\text{ ms} \rightarrow 13\text{ ms}$)**:
   - **Node B** pops from `fwd_queue` and transmits to **Node C**.
   - **Node C** receives, records the payload, and transmits an immediate ACK frame to Node B.
   - **Node B** deletes the packet from its SRAM queue upon receiving the ACK.
4. **Phase 4: Reverse Path C $\rightarrow$ B ($13\text{ ms} \rightarrow 18\text{ ms}$)**:
   - **Node C** transmits return messages or reverse telemetry to Node B.
   - **Node B** saves into its `rev_queue` in SRAM and sends an immediate ACK frame to Node C.
5. **Phase 5: Reverse Drain B $\rightarrow$ A ($18\text{ ms} \rightarrow 24\text{ ms}$)**:
   - **Node B** pops from `rev_queue` and transmits to **Node A**.
   - **Node A** receives, records the return data, and sends an ACK frame to Node B.
6. **Phase 6: Carrier Scan & Dynamic Blacklisting ($24\text{ ms} \rightarrow 25\text{ ms}$)**:
   - **Node B** tests the radio carrier (RPD). If energy is detected repeatedly on the current channel, it automatically marks the channel as jammed in the 16-byte bitmask.
   - On the very next hop, **all nodes skip the jammed frequency simultaneously in lockstep**.

---

### 3. Store-and-Forward Edge Buffering (Zero Data Loss)
- If **Node C** loses power or enters an RF dead-zone (e.g., elevator, shielded room), Node B receives no ACKs during Phase 3.
- Node B preserves the packets in its **520 KB SRAM buffer** (`fwd_queue`, up to 256 items).
- As soon as Node C returns to range, Node B detects its ACKs and flushes the entire backlog in order. **0% packet loss.**

---

## Hardware Pinout Reference

### ESP32 Nodes (Node A, Node B, Node C — Identical Wiring):
- `MOSI` $\rightarrow$ **GPIO 23**
- `MISO` $\rightarrow$ **GPIO 19**
- `SCK` $\rightarrow$ **GPIO 18**
- `CE` $\rightarrow$ **GPIO 4**
- `CSN` $\rightarrow$ **GPIO 5**
- `VCC` $\rightarrow$ **3.3V ONLY** (with 10µF capacitor)
- `GND` $\rightarrow$ **GND**

### Jammer (Arduino Mega 2560):
- `CE` $\rightarrow$ **Pin 43** (Rear Header)
- `CSN` $\rightarrow$ **Pin 45** (Rear Header)
- `SCK` $\rightarrow$ **Pin 52** (Rear Header)
- `MOSI` $\rightarrow$ **Pin 51** (Rear Header)
- `MISO` $\rightarrow$ **Pin 50** (Rear Header)
- `3.5" Touchscreen Shield` $\rightarrow$ Plugs directly onto front headers (Pins D2–D13, A0–A5)

---

## Compilation & Upload Commands

```powershell
# Node A (ESP32 Source)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_a
arduino-cli upload -p <PORT_A> --fqbn esp32:esp32:esp32 firmware/node_a

# Node B (ESP32 Relay)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_b
arduino-cli upload -p <PORT_B> --fqbn esp32:esp32:esp32 firmware/node_b

# Node C (ESP32 Destination)
arduino-cli compile --fqbn esp32:esp32:esp32 --library firmware/libraries/fhss firmware/node_c
arduino-cli upload -p <PORT_C> --fqbn esp32:esp32:esp32 firmware/node_c

# Jammer (Arduino Mega)
arduino-cli compile --fqbn arduino:avr:mega --library firmware/libraries/fhss firmware/jammer
arduino-cli upload -p <PORT_JAMMER> --fqbn arduino:avr:mega firmware/jammer
```

---

## Testing & Competition Demo Runbook

### 1. Power-Up & Synchronization Verification
1. Power up **Node B (ESP32)**. Connect phone to **`hopperb`** (password: `password`) $\rightarrow$ open `http://192.168.4.1` $\rightarrow$ observe `MASTER CLOCK: ACTIVE`.
2. Power up **Node A** and **Node C**. Within 500 ms, both acquire sync:
   - Node A Serial: `*** SYNC ACQUIRED *** Master Hop: ...`
   - Node C Serial: `*** SYNC ACQUIRED *** Master Hop: ...`
   - Node A Webpage: `SYNC STATUS: LOCKED`.

### 2. Live Bidirectional Message Dispatch
1. Connect to **`hoppera`** on your phone $\rightarrow$ open `http://192.168.4.1`.
2. Type `"Code Blue ICU"` and tap **TRANSMIT**.
3. Connect to **`hopperc`** $\rightarrow$ open `http://192.168.4.1` $\rightarrow$ observe `"Code Blue ICU"` displayed under received messages.
4. Type `"ACK: Doctor on way"` on Node C and tap **TRANSMIT** $\rightarrow$ open `hoppera` $\rightarrow$ observe return confirmation on Node A!

### 3. Store-and-Forward Dead-Zone Demo (Zero Loss)
1. **Unplug Node C** (simulating dead-zone).
2. Transmit 3 messages from Node A.
3. Check Node B (`hopperb` web portal or serial log):
   - `FWD_BUF: 3 pk`
4. **Plug Node C back in**.
5. Within 500 ms, Node B flushes all 3 packets to Node C. `FWD_BUF` drops to `0`. Zero data loss!

### 4. RF Jammer Stress Test & Dynamic Blacklisting
1. On the **Mega Jammer**, select a channel (e.g. Channel 45) and start jamming.
2. Observe Node B: `JAMMER DETECTED -> Blacklisted channel 45 (total: 1)`.
3. All three nodes automatically skip Channel 45 on the very next hop with zero packet drop.
