# hoppernet protocol

## Frame format

All RF24 payloads are fixed `MAX_FRAME_LEN = 32` bytes.

```
offset  size  field
0       1     magic = 0x5A
1       1     type   = SYNC(0x01) | DATA(0x02) | ACK(0x03)
2       1     src    = node id (1=A, 2=B, 3=C)
3       1     dst    = node id, 0 = broadcast
4       1     seq    = sequence number
5       1     hop    = hop index (mod 256)
6       1     flags  = bit0 ACK_REQ
7       1     crc    = CRC-8 over payload[0..23]
8..31   24    payload
```

### Payloads

- **SYNC** (B → broadcast): `hop_index:u32`, `master_ts_us:u32`, blacklist
  bitmap (124 bits = 16 bytes).
- **DATA** (A→B or B→C): `len:u8`, then `len` bytes of text payload.
- **ACK** (B→A or C→B): `ack_seq:u8`, `0x0D` marker.

## FHSS hop scheduling

- Channel table: 124 channels, `2..125`. PRNG = xorshift32.
- `channel_for_hop(hop, seed, blacklist)`: deterministic — same algorithm on
  every node keeps A/B/C in lockstep, **skipping blacklisted channels** so both
  ends stay synchronized.
- **Dwell = 25 ms.** Within each dwell:
  - `[0, 2 ms)` — B broadcasts **SYNC** on the current channel
  - `[2, 12 ms)` — **A → B** data, B acks
  - `[12, 25 ms)` — **B → C** data drain, C acks, jammer carrier scan

## Synchronization

- Node B is the **master clock**. Its SYNC frame carries the master microsecond
  timestamp and current hop index.
- Nodes A/C compute `clock_offset = master_ts - local_micros`, then derive hop /
  channel from master time. Each SYNC re-aligns the clock and refreshes the
  blacklist.
- **Loss of sync** → node scans all channels (`2..125`) listening for SYNC,
  re-acquires, and jumps back into the schedule.

## Jammer detection / avoidance

- Node B calls `radio.testCarrier()` (nRF24 RPD) during the quiet tail of each
  hop. Consecutive carrier detects on a channel blacklist it.
- The blacklist rides along in every SYNC frame, so A and C adopt the same
  forbidden set → the shared `channel_for_hop` algorithm keeps everyone hopping
  on identical *good* channels.

## Store-and-forward edge buffering

- Node B ACKs A immediately on receipt, then pushes into the **EdgeBuffer**
  (ram `deque` + sqlite at `run/edge_buffer.db`).
- A background drain (inside the same loop) forwards to C during the B→C window
  and pops only on C's ACK.
- **C unreachable** → packets accumulate in the buffer; on recovery they drain
  in order. Buffer survives power loss via sqlite.
