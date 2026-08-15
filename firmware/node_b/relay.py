#!/usr/bin/env python3
"""hoppernet Node B -- Relay + Store-and-Forward Edge Buffer (Raspberry Pi 4).

Master clock + jammer detection authority. Receives from Node A, buffers
locally (deque + sqlite persistence), and forwards to Node C once the
destination link recovers. Everything is logged to run/node_b.log.

Dependencies:
    sudo apt install python3-rf24 python3-pip
    pip3 install -r requirements.txt

Run:
    python3 relay.py
"""
import os
import time
import json
import sqlite3
import logging
import threading
from collections import deque

from RF24 import RF24, RF24_PA_LOW, RF24_250KBPS

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
RUN_DIR = os.path.join(BASE_DIR, "run")
LOG_PATH = os.path.join(RUN_DIR, "node_b.log")
DB_PATH = os.path.join(RUN_DIR, "edge_buffer.db")

NODE_A, NODE_B, NODE_C = 1, 2, 3
SEED = 0xC0FFEE01
DWELL_US = 25000
NUM_CHANNELS = 124
CHANNEL_BASE = 2

FRAME_SYNC = 0x01
FRAME_DATA = 0x02
FRAME_ACK = 0x03
MAGIC = 0x5A
HEADER_LEN = 8
PAYLOAD_LEN = 24
MAX_FRAME_LEN = HEADER_LEN + PAYLOAD_LEN

# ---- hopping + blacklist helpers (mirrors firmware/common/fhss.h) ----
def xorshift32(state):
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= (state >> 17)
    state ^= (state << 5) & 0xFFFFFFFF
    return state & 0xFFFFFFFF

def channel_for_hop(hop, seed, blacklist):
    state = (seed ^ (hop * 2654435761)) & 0xFFFFFFFF
    for _ in range(NUM_CHANNELS * 2):
        state = xorshift32(state)
        ch = CHANNEL_BASE + (state % NUM_CHANNELS)
        if ch not in blacklist:
            return ch
    return CHANNEL_BASE + (hop % NUM_CHANNELS)


def setup_logging():
    os.makedirs(RUN_DIR, exist_ok=True)
    logging.basicConfig(
        filename=LOG_PATH,
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )
    console = logging.StreamHandler()
    console.setFormatter(logging.Formatter("[%(levelname)s] %(message)s"))
    logging.getLogger().addHandler(console)


class EdgeBuffer:
    """Thread-safe store-and-forward buffer with sqlite persistence."""

    def __init__(self, path):
        self._lock = threading.Lock()
        self._q = deque()
        self._conn = sqlite3.connect(path, check_same_thread=False)
        self._conn.execute(
            "CREATE TABLE IF NOT EXISTS buffer ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "seq INTEGER, ts REAL, payload TEXT)"
        )
        self._conn.commit()
        self._load()

    def _load(self):
        for row in self._conn.execute(
            "SELECT seq, ts, payload FROM buffer ORDER BY id"
        ):
            self._q.append((row[0], row[1], row[2]))

    def push(self, seq, payload):
        with self._lock:
            self._conn.execute(
                "INSERT INTO buffer (seq, ts, payload) VALUES (?,?,?)",
                (seq, time.time(), payload),
            )
            self._conn.commit()
            self._q.append((seq, time.time(), payload))

    def pop(self):
        with self._lock:
            if not self._q:
                return None
            item = self._q.popleft()
            self._conn.execute(
                "DELETE FROM buffer WHERE seq=? AND payload=?",
                (item[0], item[2]),
            )
            self._conn.commit()
            return item

    def peek(self):
        with self._lock:
            return self._q[0] if self._q else None

    def size(self):
        with self._lock:
            return len(self._q)


class RelayNode:
    def __init__(self):
        self.radio = RF24(22, 0)  # CE=GPIO22, CSN=CE0
        self.blacklist = set()
        self.synced = False
        self.hop = 0
        self.acked = set()
        self.t0 = time.monotonic()
        self.buffer = EdgeBuffer(DB_PATH)
        self.jam_counter = {}
        self.seq_c = 0

    def init_radio(self):
        if not self.radio.begin():
            logging.error("RF24 init FAILED (check wiring / spi enabled)")
            raise SystemExit(1)
        self.radio.setPALevel(RF24_PA_LOW)
        self.radio.setDataRate(RF24_250KBPS)
        self.radio.setPayloadSize(MAX_FRAME_LEN)
        self.radio.setAutoAck(False)
        self.radio.startListening()
        logging.info("Node B (relay) up. Buffer: %d pending", self.buffer.size())

    def now_master_us(self):
        return int((time.monotonic() - self.t0) * 1e6)

    def build_sync(self, hop):
        frame = bytearray(MAX_FRAME_LEN)
        frame[0] = MAGIC
        frame[1] = FRAME_SYNC
        frame[2] = NODE_B
        frame[3] = 0  # broadcast
        frame[4] = 0
        frame[5] = hop & 0xFF
        frame[6] = 0
        import struct
        struct.pack_into("<II", frame, HEADER_LEN, hop, self.now_master_us())
        for ch in self.blacklist:
            idx = ch - CHANNEL_BASE
            frame[HEADER_LEN + 8 + idx // 8] |= 1 << (idx % 8)
        frame[7] = self.crc(frame, HEADER_LEN, PAYLOAD_LEN)
        return frame

    @staticmethod
    def crc(data, start, length):
        crc = 0
        for i in range(start, start + length):
            crc ^= data[i]
            for _ in range(8):
                crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
        return crc

    def valid(self, frame):
        if frame[0] != MAGIC:
            return False
        return frame[7] == self.crc(frame, HEADER_LEN, PAYLOAD_LEN)

    def scan_jam(self):
        # During the quiet tail of each hop, listen for a jamming carrier.
        carrier = self.radio.testCarrier()
        ch = self.radio.getChannel()
        self.jam_counter[ch] = self.jam_counter.get(ch, 0) + (1 if carrier else 0)
        if carrier and self.jam_counter[ch] > 3:
            self.blacklist.add(ch)
            logging.warning("Channel %d blacklisted (jamming carrier)", ch)
            self.jam_counter[ch] = 0

    def run(self):
        last_hop = -1
        while True:
            hop = self.now_master_us() // DWELL_US
            phase = self.now_master_us() % DWELL_US
            ch = channel_for_hop(hop, SEED, self.blacklist)
            if ch != self.radio.getChannel():
                self.radio.setChannel(ch)

            if hop != last_hop:
                last_hop = hop
                # start of dwell: broadcast SYNC on current channel
                self.radio.stopListening()
                self.radio.write(self.build_sync(hop))
                self.radio.startListening()

            # A -> B window
            if 2000 < phase < 12000:
                if self.radio.available():
                    buf = self.radio.read(MAX_FRAME_LEN)
                    if self.valid(buf) and buf[1] == FRAME_DATA and buf[2] == NODE_A:
                        seq = buf[4]
                        plen = buf[HEADER_LEN]
                        payload = bytes(buf[HEADER_LEN + 1:HEADER_LEN + 1 + plen])
                        if seq not in self.acked:
                            self.acked.add(seq)
                            self.buffer.push(seq, payload.decode(errors="replace"))
                            logging.info("RX from A seq=%u -> buffered (%d pending)",
                                         seq, self.buffer.size())
                        ack = bytearray(MAX_FRAME_LEN)
                        ack[0] = MAGIC
                        ack[1] = FRAME_ACK
                        ack[2] = NODE_B
                        ack[3] = NODE_A
                        ack[4] = 0
                        ack[5] = hop & 0xFF
                        ack[6] = 0
                        ack[7] = self.crc(ack, HEADER_LEN, PAYLOAD_LEN)
                        self.radio.stopListening()
                        self.radio.write(ack)
                        self.radio.startListening()

            # B -> C window: drain buffer
            if phase >= 12000:
                item = self.buffer.peek()
                if item is not None:
                    seq, ts, payload = item
                    frame = bytearray(MAX_FRAME_LEN)
                    frame[0] = MAGIC
                    frame[1] = FRAME_DATA
                    frame[2] = NODE_B
                    frame[3] = NODE_C
                    frame[4] = self.seq_c
                    frame[5] = hop & 0xFF
                    frame[6] = 0
                    plen = min(len(payload.encode()), PAYLOAD_LEN - 1)
                    frame[HEADER_LEN] = plen
                    frame[HEADER_LEN + 1:HEADER_LEN + 1 + plen] = payload.encode()[:plen]
                    frame[7] = self.crc(frame, HEADER_LEN, PAYLOAD_LEN)
                    self.radio.stopListening()
                    ok = self.radio.write(frame)
                    self.radio.startListening()

                    got_ack = False
                    deadline = time.monotonic() + 0.008
                    while time.monotonic() < deadline:
                        if self.radio.available():
                            ack = self.radio.read(MAX_FRAME_LEN)
                            if (self.valid(ack) and ack[1] == FRAME_ACK
                                    and ack[3] == NODE_B):
                                got_ack = True
                                break
                    if got_ack:
                        self.buffer.pop()
                        self.seq_c += 1
                        logging.info("FWD to C seq=%u OK (buf=%d)",
                                     seq, self.buffer.size())
                    else:
                        logging.warning("FWD to C seq=%u NO-ACK (C unreachable?) "
                                        "kept in buffer", seq)
                    self.scan_jam()

            time.sleep(0.002)


if __name__ == "__main__":
    setup_logging()
    node = RelayNode()
    node.init_radio()
    try:
        node.run()
    except KeyboardInterrupt:
        logging.info("Node B stopped. Pending in buffer: %d", node.buffer.size())
