# HopperNet Protocol Specification (v4.0)

## 1. Frame Structure

All nRF24L01+ payloads are fixed at **32 bytes** (`MAX_FRAME_LEN = 32`) to ensure fixed transmission air-time and deterministic slot boundaries.

```
Offset  Size  Field        Description
0       1     magic        Protocol identifier byte: 0x5A
1       1     type         Frame Type: SYNC (0x01), DATA (0x02), ACK (0x03), STATUS (0x05)
2       1     src          Source Node ID: 1=Node A, 2=Node B, 3=Node C
3       1     dst          Destination ID: 1=A, 2=B, 3=C, 0=Broadcast
4       1     seq          Sequence counter (0–255)
5       1     hop_index    Hop counter mod 256
6       1     flags        bit 0: ACK_REQ
7       1     crc          CRC-8 computed over payload[0..23] (poly 0x07)
8..31   24    payload      Frame-specific payload data
```

---

## 2. Payload Layouts

### A. SYNC Frame (`type = 0x01`, Broadcast from Node B)
- `payload[0..3]`: 32-bit Hop Index (`uint32_t`)
- `payload[4..7]`: 32-bit Master Microsecond Timestamp (`uint32_t`)
- `payload[8..23]`: 16-byte (128-bit) Blacklist Bitmap covering channels 2–125

### B. DATA Frame (`type = 0x02`, A ➔ B or B ➔ C)
- `payload[0]`: Payload length in bytes ($L \le 23$)
- `payload[1..L]`: UTF-8 Message or telemetry string ($L$ bytes)
- `payload[L+1..23]`: Zero padding

### C. ACK Frame (`type = 0x03`, B ➔ A or C ➔ B)
- `payload[0]`: Acknowledged sequence number
- `payload[1]`: Status code (`0x0D` = SUCCESS / STORED)

---

## 3. Hopping Schedule & Phase Breakdown

- **Total Dwell Time**: 25,000 µs (25 ms) per channel hop.
- **Spectrum**: 124 RF channels (channels 2 to 125, $2400 + \text{ch}$ MHz).

```
0 ms              2 ms                     12 ms                                 25 ms
├── SYNC Phase ───┼── A ➔ B Data Window ───┼── B ➔ C Data Drain & Carrier Scan ──┤
│   B Broadcasts  │   A sends to B         │   B sends to C, C acks              │
│   Sync Beacon   │   B sends ACK          │   B tests carrier in quiet tail     │
```

---

## 4. Deterministic Channel Selection Algorithm

```c
uint8_t channel_for_hop(uint32_t hop, uint32_t seed, const uint8_t *blacklist) {
    uint32_t state = seed ^ (hop * 2654435761u);
    for (int attempt = 0; attempt < NUM_CHANNELS * 2; attempt++) {
        state = xorshift32(state);
        uint8_t ch = (uint8_t)(CHANNEL_BASE + (state % NUM_CHANNELS));
        if (!blacklist_get(blacklist, ch)) return ch;
    }
    return (uint8_t)(CHANNEL_BASE + (hop % NUM_CHANNELS));
}
```
When a channel is blacklisted, every node running this function automatically skips it in lockstep without exchanging negotiation packets.
