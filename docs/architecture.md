# System Architecture — MedRelay (HopperNet)

## 1. High-Level Topology

```
                              SUPABASE CLOUD & DASHBOARD
                      ┌─────────────────────────────────────────┐
                      │    Live 124-Channel Spectrum Heatmap    │
                      │    Real-time Message Dispatch & Feed    │
                      │    Telemetry & Edge Buffer Depth Gauge  │
                      └────────────────────┬────────────────────┘
                                           │ HTTPS REST & WebSockets
                                 ┌─────────┴─────────┐
                                 │   WiFi Network    │
                                 │ (hoppernet/pass)  │
                                 └─────────┬─────────┘
                                           │
             ┌─────────────────────────────┼─────────────────────────────┐
             │                             │                             │
    ┌────────▼────────┐           ┌────────▼────────┐           ┌────────▼────────┐
    │  Node A (ESP32) │           │  Node B (ESP32) │           │  Node C (ESP32) │
    │  Source Node    │           │  Relay & Master │           │  Destination    │
    └────────┬────────┘           └────────┬────────┘           └────────┬────────┘
             │                             │                             │
             └────── SYNC / DATA / ACK ────┴────── DATA / ACK ───────────┘
                       2.4 GHz FHSS Mesh (124 Channels, 25ms Dwell)
                                           ▲
                                           │ RF Interference
                                  ┌────────┴────────┐
                                  │  Jammer (ESP32) │
                                  │ Active Adversary│
                                  └─────────────────┘
```

---

## 2. Core Functional Pillars

### A. Frequency Hopping Spread Spectrum (FHSS)
- **124 Available RF Channels**: Spanning 2.402 GHz to 2.525 GHz (`CHANNEL_BASE = 2`).
- **Synchronous PRNG**: `xorshift32` generator seeded with `0xC0FFEE01`.
- **Dwell Time**: 25 ms per channel hop.
- **Dwell Phase Windows**:
  - `[0, 2 ms)`: **SYNC Window** — Node B (Master) broadcasts timestamp + hop counter + blacklist bitmap.
  - `[2, 12 ms)`: **A ➔ B Window** — Node A transmits data to Node B; Node B returns an immediate ACK.
  - `[12, 25 ms)`: **B ➔ C Window & Scan** — Node B transmits buffered frames to Node C; Node C returns ACK; Node B conducts quiet-tail carrier detection (`testCarrier()`).

### B. Dynamic Channel Blacklisting & Jammer Avoidance
- Node B monitors the nRF24 Received Power Detector (RPD) register during quiet slots.
- Consecutive carrier hits flag a channel as jammed.
- Blacklisted channels are set in a 16-byte bitmap broadcast in every SYNC beacon.
- All nodes run the shared `channel_for_hop()` algorithm, deterministically skipping blacklisted channels in lockstep without losing timing sync.

### C. Persistent Store-and-Forward Edge Buffering
- Node B acts as the intelligent buffer relay.
- When Node A sends a frame, Node B ACKs immediately upstream and enqueues the packet into its in-memory circular buffer and **SPIFFS flash filesystem**.
- Frames are only removed from Node B's storage when Node C issues a positive downstream ACK.
- If Node C goes offline or experiences severe jamming, packets safely accumulate in Node B and drain in strict FIFO order once Node C recovers.

### D. Dual-Core Asynchronous Architecture (FreeRTOS)
Every ESP32 node divides duties across hardware CPU cores:
- **Core 1 (Real-Time RF Engine)**: Manages microsecond-precise SPI transactions, dwell timing, carrier scans, and radio interrupts without jitter.
- **Core 0 (Cloud & Network Engine)**: Manages WiFi connectivity, HTTPS REST polling/pushing to Supabase, and real-time telemetry streaming.

---

## 3. Database & Cloud Schema (Supabase)

| Table | Purpose |
| :--- | :--- |
| `messages` | Outbound dispatch queue (Phone ➔ Node A ➔ Mesh). Status transitions: `pending` ➔ `sent` ➔ `delivered`. |
| `received_messages` | Inbound delivery log pushed by Node C upon physical receipt. |
| `telemetry` | Live health stats from Nodes A, B, and C (sent, acked, received, buffer depth, channel, hop). |
| `blacklist_events` | Interference log capturing jammed channel numbers and timestamps. |
