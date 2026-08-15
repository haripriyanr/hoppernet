# hoppernet

**Jammer-Resilient FHSS Mesh with Edge Buffering** — a three-node wireless
system where **Node A** talks to **Node C** through **Node B**, a smart
store-and-forward relay.

- **Node A (source)** — ESP32 + nRF24L01+
- **Node B (relay / edge buffer)** — Raspberry Pi 4 + nRF24L01+
- **Node C (destination)** — Arduino Due + nRF24L01+

The radio mesh uses pseudo-random **Frequency Hopping Spread Spectrum (FHSS)**:
all nodes hop through a shared channel table in lockstep. When a channel is
jammed, a node blacklists it and both ends deterministically skip it, keeping
the link alive. When Node C goes unreachable, Node B **buffers packets locally**
(ram + sqlite) and reliably delivers them once connectivity returns.

## Repository layout

```
firmware/
  common/fhss.h      shared protocol: frame, CRC, PRNG, channel table, blacklist
  node_a/            ESP32 source (Node A)
  node_c/            Arduino Due destination (Node C)
  node_b/            Raspberry Pi relay + edge buffer (Python)
tools/
  serial_logger.py   capture Node A/C serial logs into run/
docs/
  architecture.md    system design
  protocol.md        FHSS frame / sync / blacklist protocol
  wiring.md          nRF24L01+ wiring for all three boards
run/                 runtime logs + edge-buffer DB (gitignored)
```

## Quick start (3 steps)

1. **Wiring** — follow `docs/wiring.md` for each board.
2. **Node B (Pi)** — run `scripts/setup_pi.sh` on the Pi (enables SPI, installs
   RF24, sets up a systemd service), or run `relay.py` manually.
3. **Node A & C** — flash with Arduino CLI:
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32 firmware/node_a
   arduino-cli upload -p COM8 --fqbn esp32:esp32:esp32 firmware/node_a
   arduino-cli compile --fqbn arduino:sam:arduino_due_x_dbg firmware/node_c
   arduino-cli upload -p COM9 --fqbn arduino:sam:arduino_due_x_dbg firmware/node_c
   ```
   Capture logs: `python tools/serial_logger.py --port COM8 --name node_a`

## Diagnosis

Every node leaves logs behind:

- Node A / C → serial output, captured to `run/node_a.log` / `run/node_c.log`
- Node B → `firmware/node_b/run/node_b.log` (on the Pi)
- Edge buffer contents survive restarts in `run/edge_buffer.db`
