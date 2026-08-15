# Architecture

## Topology

```
         FHSS radio mesh (2.4 GHz, nRF24L01+)
     ┌──────────┐  SYNC + DATA + ACK  ┌──────────────┐
     │ Node A   │ ───────────────────▶ │ Node B       │
     │ ESP32    │◀─────────────────── │ RPi 4 relay  │
     │ source   │      ACK            │ edge buffer  │
     └──────────┘                     └──────┬───────┘
                                             │ DATA + ACK (FHSS)
                                      ┌──────▼───────┐
                                      │ Node C       │
                                      │ Arduino Due  │
                                      │ destination  │
                                      └──────────────┘
```

- **Node A** originates messages each dwell and waits for B's ACK.
- **Node B** is the FHSS master clock, jammer authority, and store-and-forward
  relay. It ACKs A, buffers locally, and drains toward C.
- **Node C** receives from B, ACKs, and prints deliveries over serial.

## Edge buffering (the "pipe")

The core reliability feature lives on Node B:

1. A transmits → B ACKs **immediately** (fast, lossless upstream handshake).
2. B pushes into a thread-safe `EdgeBuffer` (ram + sqlite, survives reboot).
3. B drains the buffer to C during the B→C dwell window, popping **only** on C's ACK.
4. If C is offline (no ACK), the buffer **holds and retries** — nothing is dropped.

## Jammer resilience

- Deterministic FHSS: `channel_for_hop(hop, seed, blacklist)` computes the same
  channel on all three nodes.
- Node B detects a jammed channel via RPD carrier scans and blacklists it.
- Blacklist propagates in SYNC frames → all nodes skip the bad channel in
  lockstep. Hopping continues on good channels.

## Time synchronization

- B is master (monotonic clock). SYNC carries `master_ts` + hop index.
- A/C compute a clock offset and derive `hop = master_time / dwell`, so channel
  switches happen at the same instant on every node.
- Losing sync triggers a full channel scan; re-acquisition is automatic.

## Logging & diagnostics

| Node | Medium | Location |
|------|--------|----------|
| A    | serial | `run/node_a.log` (via `tools/serial_logger.py`) |
| B    | file   | `firmware/node_b/run/node_b.log` |
| C    | serial | `run/node_c.log` |
| Buffer state | sqlite | `firmware/node_b/run/edge_buffer.db` |
