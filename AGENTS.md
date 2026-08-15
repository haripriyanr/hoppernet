# AGENTS.md — HopperNet (Spectrum-Pipe)

## Project overview

HopperNet is a **Jammer-Resilient FHSS Mesh with Edge Buffering**. Three
microcontroller-based nodes communicate via nRF24L01+ 2.4 GHz radio using a
pseudo-random Frequency Hopping Spread Spectrum protocol. When a channel is
jammed, the mesh detects the interference, blacklists the channel, and hops to
a clean one. When the destination goes unreachable, the relay buffers packets
locally (RAM + SQLite) and delivers them once connectivity returns.

A fourth node acts as an **active RF jammer** (nRF24L01+ on an ESP32) to test
the resilience mechanism.

## Dependencies

### Hardware

| Device | Board | Module | Role |
|--------|-------|--------|------|
| Node A | ESP32 (COM8) | nRF24L01+ #1 | Source — sends frames |
| Node B | Raspberry Pi 4 | nRF24L01+ #2 | Relay + edge buffer (master clock) |
| Node C | Arduino Due (COM9) | nRF24L01+ #3 | Destination — receives frames |
| Jammer | ESP32 (separate board) | nRF24L01+ #4 | RF jammer — transmits on random channels |
| — | — | 4× 10µF caps | Brown-out protection at each module |

### Software (on this Windows PC)

- `arduino-cli` — `C:\Program Files\Arduino CLI\arduino-cli.exe`
- ESP32 core 3.3.11 — installed via arduino-cli
- Arduino SAM core — installed via arduino-cli
- RF24 library 1.6.2 — `C:\Users\harip\Documents\Arduino\libraries\RF24`
- esptool — `C:\Users\harip\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe`
- Python 3.14 + pyserial — `C:\Users\harip\AppData\Local\Python\pythoncore-3.14-64`

### Software (on Raspberry Pi)

- Raspberry Pi OS Lite 64-bit (Trixie)
- WiFi: SSID `hoppernet`, password `password`
- SSH: `pi@nodeb` / `hoppernet123`
- Cloud-init auto-provisions on first boot (see `F:\user-data`)
- RF24 built from source on every boot via `scripts/onboot.sh`
- Relay runs as systemd service `hoppernet-nodeb`

## Protocol (fhss.h)

- **124 channels**: 2–125 (nRF24L01+ register values)
- **PRNG**: xorshift32, seed `0xC0FFEE01`
- **Dwell time**: 25 ms per channel
- **Phases per hop** (25 ms window):
  - `[0, 2 ms)` — SYNC broadcast (Node B → all)
  - `[2, 12 ms)` — A → B data window
  - `[12, 25 ms)` — B → C data window + jammer scan
- **Frame**: 32 bytes fixed — magic `0x5A`, type, src, dst, seq, hop_index,
  flags, crc8, 24-byte payload
- **Blacklist**: 16-byte bitmap broadcast in SYNC frame, all nodes skip
  blacklisted channels deterministically
- **Jammer detection**: `radio.testCarrier()` (RPD register) during quiet tail
  of each hop

## File structure

```
hoppernet/
├── firmware/
│   ├── libraries/
│   │   └── fhss/src/fhss.h        # shared protocol (Arduino library)
│   ├── node_a/node_a.ino           # ESP32 source node
│   ├── node_b/
│   │   ├── relay.py                # Pi relay + edge buffer
│   │   ├── requirements.txt
│   │   └── run/                    # logs + sqlite DB (gitignored)
│   ├── node_c/node_c.ino           # Arduino Due destination node
│   └── jammer/jammer.ino           # ESP32 nRF24L01+ RF jammer
├── tools/
│   └── serial_logger.py            # capture serial logs to run/
├── scripts/
│   ├── setup_pi.sh                 # manual Pi provisioning
│   └── onboot.sh                   # every-boot update + rebuild RF24
├── docs/
│   ├── architecture.md             # system design
│   ├── protocol.md                 # FHSS frame / sync / blacklist
│   └── wiring.md                   # pinout for all boards + jammer
├── run/                            # local logs (gitignored)
├── .gitignore
├── README.md
└── AGENTS.md                       # this file
```

## Build & flash

### Node A (ESP32, COM8)

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/node_a
arduino-cli upload -p COM8 --fqbn esp32:esp32:esp32 firmware/node_a
```

If "Wrong boot mode (0x13)" error, flash with esptool directly:

```powershell
esptool --before no_reset --chip esp32 --port COM8 --baud 921600 `
  write_flash 0x0 firmware/node_a_build/node_a.ino.merged.bin
```

### Node C (Arduino Due, COM9)

```powershell
arduino-cli compile --fqbn arduino:sam:arduino_due_x_dbg firmware/node_c
arduino-cli upload -p COM9 --fqbn arduino:sam:arduino_due_x_dbg firmware/node_c
```

### Jammer (ESP32, separate board)

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/jammer
arduino-cli upload -p <JAMMER_PORT> --fqbn esp32:esp32:esp32 firmware/jammer
```

### Node B (Raspberry Pi)

SD card is pre-configured via cloud-init. On first boot:
1. Pi connects to WiFi `hoppernet`/`password`
2. `firstboot.sh` installs build tools, clones repo, creates systemd services
3. `hoppernet-update.service` runs `onboot.sh` every boot: apt update+upgrade,
   git pull RF24 + hoppernet, rebuild RF24
4. `hoppernet-nodeb.service` starts relay after update completes

Manual re-provisioning: `sudo bash scripts/setup_pi.sh`

## Test procedure

### 2-node test (2 modules)

1. Wire nRF24L01+ #1 to ESP32 (Node A), #2 to Pi (Node B)
2. Boot Pi → wait for `systemctl status hoppernet-nodeb` to show active
3. Open serial monitor on COM8 (115200 baud) — see `SYNC acquired` + stats
4. Verify frames are being sent and buffered

### 3-node test (3 modules)

1. Add nRF24L01+ #3 to Arduino Due (Node C)
2. Flash Due, open serial on COM9 (115200 baud)
3. Verify: A sends → B receives + buffers → B forwards → C receives

### Jammer test (4 modules)

1. Flash jammer.ino to a 4th ESP32 with nRF24L01+ #4
2. Wire jammer module, open serial (115200 baud)
3. Start nodes (A, B, C) — verify normal communication
4. On jammer serial: type `j` to start jamming
5. Observe on Node B serial: channel blacklisted messages
6. Observe on Node A/C serial: hop index changes, channels skip blacklisted ones
7. Type `j` on jammer to stop — verify buffer drains and communication resumes
8. Type `b` on jammer to sweep all 124 channels — maximum disruption test

### Serial monitoring

```powershell
python tools/serial_logger.py --port COM8 --name node_a
python tools/serial_logger.py --port COM9 --name node_c
```

Pi logs: `ssh pi@nodeb 'tail -f /home/pi/hoppernet/firmware/node_b/run/node_b.log'`

## Design decisions

- **Bare vs PA/LNA modules**: Use bare for Node B (Pi) to avoid brown-out on
  the Pi's limited 3.3V rail. PA/LNA is fine for ESP32 and Due (beefier regs).
  PA/LNA recommended for jammer (stronger TX = more effective jamming).
- **Pi builds RF24 from source on every boot**: Ensures latest version, avoids
  stale packages, keeps the node self-updating.
- **Edge buffer persists to SQLite**: Survives relay restarts, survives power
  loss. Deque for in-memory speed, SQLite for durability.
- **No WiFi/BLE for jamming**: The nRF24L01+ jammer targets the exact FHSS
  band (channels 2–125) for precise, controllable interference testing.
