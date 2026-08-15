# hoppernet

**Jammer-Resilient FHSS Mesh with Edge Buffering** — a three-node wireless
system where **Node A** talks to **Node C** through **Node B**, a smart
store-and-forward relay. A fourth node acts as an **active RF jammer** to test
resilience.

## Hardware

| Device | Board | Module | Role |
|--------|-------|--------|------|
| Node A | ESP32 | nRF24L01+ | Source — sends frames |
| Node B | Raspberry Pi 4 | nRF24L01+ | Relay + edge buffer (master clock) |
| Node C | Arduino Due | nRF24L01+ | Destination — receives frames |
| Jammer | ESP32 | nRF24L01+ | RF jammer — transmits on random channels |

## Shopping list (4 modules)

| # | Item | Purpose |
|---|------|---------|
| 1 | nRF24L01+ module | Node A |
| 2 | nRF24L01+ module (bare preferred) | Node B (Pi) |
| 3 | nRF24L01+ module | Node C |
| 4 | nRF24L01+ module (PA/LNA preferred) | Jammer (stronger TX) |
| 5 | 4× 10µF electrolytic caps | Brown-out protection at each module |
| 6 | Jumper wires (M-M or M-F) | Connections |

## Repository layout

```
firmware/
  libraries/fhss/src/fhss.h    shared protocol (Arduino library)
  node_a/                      ESP32 source (Node A)
  node_b/                      Pi relay + edge buffer (Python)
  node_c/                      Arduino Due destination (Node C)
  jammer/                      ESP32 nRF24L01+ RF jammer
tools/
  serial_logger.py             capture serial logs to run/
docs/
  architecture.md              system design
  protocol.md                  FHSS frame / sync / blacklist
  wiring.md                    pinout for all boards + jammer
scripts/
  setup_pi.sh                  manual Pi provisioning
  onboot.sh                    every-boot update + rebuild RF24
```

## Quick start

1. **Wire** — follow `docs/wiring.md` for each board
2. **Flash Node A** — `arduino-cli upload -p COM8 --fqbn esp32:esp32:esp32 firmware/node_a`
3. **Flash Node C** — `arduino-cli upload -p COM9 --fqbn arduino:sam:arduino_due_x_dbg firmware/node_c`
4. **Boot Pi** — SD card auto-provisions, WiFi connects, relay starts
5. **Flash Jammer** — `arduino-cli upload -p <PORT> --fqbn esp32:esp32:esp32 firmware/jammer`

## Test procedure

### 2-node (Node A + Node B)

1. Wire 2 modules (ESP32 + Pi), boot Pi
2. Open serial on COM8 — see `SYNC acquired` + stats
3. Verify frames sent and buffered

### 3-node (add Node C)

1. Wire 3rd module to Due, flash Due
2. Open serial on COM9 — see frames received via relay

### Jammer test (4 nodes)

1. Flash jammer to 4th ESP32 with 4th module
2. Start all nodes, verify normal communication
3. On jammer serial: `j` to start jamming
4. Observe channel blacklisting + hopping on node serials
5. `j` to stop — buffer drains, communication resumes
6. `b` to sweep all 124 channels — maximum disruption

### Serial monitoring

```powershell
python tools/serial_logger.py --port COM8 --name node_a
python tools/serial_logger.py --port COM9 --name node_c
```

Pi logs: `ssh pi@nodeb 'tail -f /home/pi/hoppernet/firmware/node_b/run/node_b.log'`
