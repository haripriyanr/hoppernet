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
  - Connect to **`hopperb`** $\rightarrow$ open `http://192.168.4.1` to view relay buffer depth (`fwd_buf`, `jam_count`).
  - Connect to **`hopperc`** $\rightarrow$ open `http://192.168.4.1` to view received messages.
- **Local Desktop App (`app/main.py`)**:
  - Connects to all plugged USB COM ports simultaneously.
  - Auto-detects device roles (`NODE_A`, `NODE_B`, `NODE_C`, `JAMMER`).
  - Provides a unified web interface on `http://localhost:8000`.

---

### 2. 50-Millisecond Slotted FHSS Protocol Flow (Bidirectional)

Every 50 ms superframe hops across the 124 available frequencies ($2.402\text{ GHz to }2.525\text{ GHz}$). The A→B hop rides `FHSS_SEED_AB`; the B→C / C→B hop rides `FHSS_SEED_BC` — Node B and Node C switch channels in lockstep between forward and return slots:

```
0 ms              4 ms                     16 ms                    28 ms         40 ms    50 ms
├── SYNC Phase ───┼── FORWARD: A ➔ B ──────┼── DRAIN: B ➔ C ───────┼── RETURN: C ➔ B ──┼─ B ➔ A ─┤
│ Node B Beacon   │ A sends to B (4-16ms)  │ B sends to C (16-28ms)│ C replies to B    │ B drains │
│ (anchors + Ch0) │ B custody-ACKs         │ C delivery-ACKs       │ (28-40ms)         │ to A     │
```

#### Phase Breakdown:
1. **Phase 1: SYNC Beacon ($0\text{ ms} \rightarrow 4\text{ ms}$)**:
   - **Node B** (Master Clock) transmits a `FT_SYNC` packet containing:
     - Current Superframe Index (32-bit)
     - 16-byte Active Dynamic Blacklist Mask
   - **Node A & Node C** listen. When captured, they compute `clock_offset = master_sf - micros()` and hop in exact mathematical lockstep.
2. **Phase 2: Forward Path A $\rightarrow$ B ($4\text{ ms} \rightarrow 16\text{ ms}$)**:
   - **Node A** transmits its queued outbound data frame (`FT_DATA`) to Node B.
   - **Node B** receives, saves into its `qAC` custody queue in SRAM, and sends an immediate `FT_CUSTODY` ACK frame back to Node A.
3. **Phase 3: Forward Drain B $\rightarrow$ C ($16\text{ ms} \rightarrow 28\text{ ms}$)**:
   - **Node B** pops from `qAC` and transmits to **Node C**.
   - **Node C** receives, records the payload, and transmits an immediate `FT_DELIVERY` ACK/NACK frame to Node B.
   - **Node B** deletes the packet from its SRAM queue upon receiving the ACK.
4. **Phase 4: Return Path C $\rightarrow$ B ($28\text{ ms} \rightarrow 40\text{ ms}$)**:
   - **Node C** transmits its queued reply/telemetry frame (`FT_DATA`) to **Node B** and listens inline for the immediate `FT_CUSTODY` ACK.
   - **Node B** receives, saves into its `qCA` custody queue in SRAM, and sends the custody ACK back to C.
5. **Phase 5: Return Drain B $\rightarrow$ A ($40\text{ ms} \rightarrow 48\text{ ms}$)**:
   - **Node B** pops from `qCA` and transmits to **Node A**.
   - **Node A** reassembles the return message, sends an immediate `FT_DELIVERY` ACK so Node B clears custody, and displays it in the web portal.
   - Node B runs the **RPD Jammer probe** in the guard window (48–50 ms) and automatically blacklists jammed channels in the 16-byte bitmask, distributed on the next SYNC beacon.

---

### 3. Store-and-Forward Edge Buffering (Zero Data Loss)
- If **Node C** loses power or enters an RF dead-zone (e.g., elevator, shielded room), Node B receives no `FT_DELIVERY` ACKs during Phase 3.
- Node B preserves the packets in its **SRAM buffer** (`qAC`, up to 64 items).
- As soon as Node C returns to range, Node B detects its ACKs and flushes the entire backlog in order. **0% packet loss.**
- The **reverse path is buffered symmetrically**: `qCA` holds Node C replies until Node A returns to range, then drains in order with the same custody/delivery ACK handshake.

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

### 2. Live Forward Message Dispatch
1. Connect to **`hoppera`** on your phone $\rightarrow$ open `http://192.168.4.1`.
2. Type `"Code Blue ICU"` and tap **TRANSMIT**.
3. Connect to **`hopperc`** $\rightarrow$ open `http://192.168.4.1` $\rightarrow$ observe `"Code Blue ICU"` displayed under received messages.

### 3. Store-and-Forward Dead-Zone Demo (Zero Loss)
1. **Unplug Node C** (simulating dead-zone).
2. Transmit 3 messages from Node A.
3. Check Node B (`hopperb` web portal or serial log):
   - `FWD_BUF: 3 pk`
4. **Plug Node C back in**.
5. Within 500 ms, Node B flushes all 3 packets to Node C. `FWD_BUF` drops to `0`. Zero data loss!

### 4. Live Return Path (C → B → A Talk-Back)
1. Connect to **`hopperc`** $\rightarrow$ open `http://192.168.4.1`.
2. Type `"Vitals stable"` and tap **TRANSMIT** (or START RETURN LOOP to auto-send).
3. Connect to **`hoppera`** $\rightarrow$ observe `"Vitals stable"` under RETURN MESSAGES.
4. Check Node B (`hopperb`): `Rev Custody C→A` and `Delivered C→A` counters increment.

### 5. RF Jammer Stress Test & Dynamic Blacklisting
1. On the **Mega Jammer**, select a channel (e.g. Channel 45) and start jamming.
2. Observe Node B: `JAMMER DETECTED -> Blacklisted channel 45 (total: 1)`.
3. All three nodes automatically skip Channel 45 on the very next hop with zero packet drop.
