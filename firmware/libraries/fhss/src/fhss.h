#ifndef FHSS_H
#define FHSS_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

// ---------------- Hardware Pinout (ESP32 VSPI) ----------------
#ifndef RF_CE_PIN
#define RF_CE_PIN 4
#endif
#ifndef RF_CSN_PIN
#define RF_CSN_PIN 5
#endif
#ifndef RF_SCK_PIN
#define RF_SCK_PIN 18
#endif
#ifndef RF_MISO_PIN
#define RF_MISO_PIN 19
#endif
#ifndef RF_MOSI_PIN
#define RF_MOSI_PIN 23
#endif

// ---------------- Node Identifiers ----------------
#define NODE_A            1
#define NODE_B            2
#define NODE_C            3

// ---------------- 1250 us RF Hop Slot Timing ----------------
// Master Superframe advances every 1250 us (800 hops/sec).
// Micro-slot cycle: 6 slots = 7.5 ms full bidirectional mesh exchange:
// Slot 0 (SYNC)   : Node B broadcasts SYNC + Blacklist + Backpressure on Anchor/CH0
// Slot 1 (A->B)   : Node A transmits forward data fragment to Node B (AB channel)
// Slot 2 (B->C)   : Node B forward-drains queued fragment to Node C (BC channel)
// Slot 3 (C->B)   : Node C transmits return telemetry fragment to Node B (BC channel)
// Slot 4 (B->A)   : Node B return-drains queued fragment to Node A (AB channel)
// Slot 5 (MAINT)  : RPD Jammer detector scan & link health maintenance
#define SUPERFRAME_US     1250UL      // 1250 us RF hop/dwell slot
#define HOPS_PER_SEC      800         // Actual RF hop rate
#define DISPLAY_HOPS_SEC  20          // WebUI spectrum stream sample size
#define MICRO_SLOTS       6           // 6-slot bidirectional round-robin
#define SLOT_SYNC         0
#define SLOT_AB_RX        1           // Node B perspective: receive from A
#define SLOT_BC_TX        2           // Node B perspective: drain to C
#define SLOT_BC_RX        3           // Node B perspective: receive from C
#define SLOT_AB_TX        4           // Node B perspective: drain to A
#define SLOT_MAINT        5           // RPD probe / maintenance
#define SLOT_GUARD_US     80UL        // Reserved tail inside each 1250 us slot

static inline uint8_t microSlot(uint32_t sf) {
    return (uint8_t)(sf % MICRO_SLOTS);
}

// ---------------- RF Spectrum Configuration ----------------
#define NUM_SYNC_ANCHORS  4
// Anchors live at 2.483-2.513 GHz, above all Wi-Fi 1/6/11 bands (ends ~2.473 GHz)
static const uint8_t SYNC_ANCHORS[NUM_SYNC_ANCHORS] = { 83, 93, 103, 113 };

static inline uint8_t getSyncChannel(uint32_t sf) {
    return SYNC_ANCHORS[(sf / MICRO_SLOTS) % NUM_SYNC_ANCHORS];
}

#define RF_CHANNEL_SYNC   0        // Primary rendezvous / recovery channel
#define RF_CHANNEL_FIRST  2        // Channels 2..125 = 124 data channels
#define RF_CHANNEL_COUNT  124
#define RF_CHANNEL_LAST   125
#define BLACKLIST_BYTES   16       // 128 bits, 124 used
#define RF_PAYLOAD_SIZE   32
#define DATA_PLAINTEXT_MAX 8       // 8 bytes plaintext payload per frame fragment
#define GCM_TAG_SIZE      8        // 8-byte Poly1305 authentication tag
#define TAG_SIZE          GCM_TAG_SIZE
#define DATA_FLAG_E2E      0x01    // Payload is end-to-end encrypted
#define DATA_FLAG_RECOVERED 0x02   // Relay delivered a fragment from backlog
#define DATA_FLAG_CRITICAL 0x04    // Application marks the message as critical

// Two independent PRNG sequences: A->B (FHSS_SEED_AB) and B->C / C->B (FHSS_SEED_BC)
#define FHSS_SEED_AB      0x41B2D931UL
#define FHSS_SEED_BC      0xC73A91E5UL

// Backwards compatibility aliases
#define BLACKLIST_SIZE    BLACKLIST_BYTES
#define CHANNEL_BASE      RF_CHANNEL_FIRST
#define NUM_CHANNELS      RF_CHANNEL_COUNT
#define FHSS_SEED         FHSS_SEED_AB
#define DWELL_US          SUPERFRAME_US
#define NUM_ANCHOR_CHANNELS NUM_SYNC_ANCHORS
static const uint8_t ANCHOR_CHANNELS[NUM_ANCHOR_CHANNELS] = { 83, 93, 103, 113 };

// ---------------- Protocol Constants & Packet Structures ----------------
#define SP_MAGIC          0x5350
#define SP_VERSION        2

enum FrameType : uint8_t {
    FT_SYNC = 1,
    FT_DATA = 2,
    FT_ACK = 3,
    FT_CUSTODY = 4,
    FT_DELIVERY = 5,
    FT_MAP = 6
};

struct __attribute__((packed)) DataFrame {
    uint16_t magic;       // 2
    uint8_t  version;     // 1
    uint8_t  type;        // 1
    uint8_t  src;         // 1
    uint8_t  dst;         // 1
    uint32_t sf;          // 4 (Superframe counter)
    uint16_t msgId;       // 2
    uint8_t  frag;        // 1
    uint8_t  total;       // 1
    uint8_t  flags;       // 1
    uint8_t  len;         // 1
    uint8_t  ciphertext[DATA_PLAINTEXT_MAX]; // 8
    uint8_t  tag[GCM_TAG_SIZE];              // 8
}; // Exactly 32 bytes

struct __attribute__((packed)) AckFrame {
    uint16_t magic;       // 2
    uint8_t  version;     // 1
    uint8_t  type;        // 1
    uint8_t  src;         // 1
    uint8_t  dst;         // 1
    uint32_t sf;          // 4
    uint16_t msgId;       // 2
    uint8_t  frag;        // 1
    uint8_t  code;        // 1 (1 = CUSTODY, 2 = DELIVERY)
    uint32_t masterUs;    // 4 (Master microsecond timestamp)
    uint8_t  bp_fwd;      // 1 (Node B forward backpressure: 0=OK, 1=Throttled)
    uint8_t  bp_rev;      // 1 (Node B reverse backpressure: 0=OK, 1=Throttled)
    uint8_t  reserved[12];// 12 -> Total: 32 bytes
}; // Exactly 32 bytes

struct __attribute__((packed)) SyncFrame {
    uint16_t magic;       // 2
    uint8_t  version;     // 1
    uint8_t  type;        // 1
    uint8_t  src;         // 1
    uint8_t  dst;         // 1
    uint32_t sf;          // 4
    uint16_t mapVersion;  // 2
    uint8_t  blacklist[BLACKLIST_BYTES]; // 16
    uint16_t masterUs16;  // 2 (Master microsecond timestamp lower 16-bits)
    uint8_t  bp_fwd;      // 1 (Node B forward backpressure: 0=OK, 1=Throttled)
    uint8_t  bp_rev;      // 1 (Node B reverse backpressure: 0=OK, 1=Throttled)
}; // Exactly 32 bytes

static_assert(sizeof(DataFrame) == 32, "DataFrame must be exactly 32 bytes");
static_assert(sizeof(AckFrame) == 32, "AckFrame must be exactly 32 bytes");
static_assert(sizeof(SyncFrame) == 32, "SyncFrame must be exactly 32 bytes");

// ---------------- Demo E2E Encryption Key (256-bit ChaCha20) ----------------
// Node A and Node C use this key for ChaCha20-Poly1305. Node B never decrypts payloads.
static const uint8_t E2E_KEY[32] = {
    0x53, 0x50, 0x2D, 0x45, 0x32, 0x45, 0x2D, 0x44,
    0x45, 0x4D, 0x4F, 0x2D, 0x32, 0x30, 0x32, 0x36,
    0x48, 0x6F, 0x70, 0x70, 0x65, 0x72, 0x4E, 0x65,
    0x74, 0x2D, 0x32, 0x30, 0x32, 0x36, 0x2D, 0x42
};

// ---------------- Blacklist Bitset Helpers ----------------
static inline void blClear(uint8_t *b) {
    memset(b, 0, BLACKLIST_BYTES);
}

static inline bool blGet(const uint8_t *b, uint8_t ch) {
    if (ch < RF_CHANNEL_FIRST || ch > RF_CHANNEL_LAST) return false;
    uint8_t i = ch - RF_CHANNEL_FIRST;
    return (b[i >> 3] >> (i & 7)) & 1U;
}

static inline void blSet(uint8_t *b, uint8_t ch, bool on = true) {
    if (ch < RF_CHANNEL_FIRST || ch > RF_CHANNEL_LAST) return;
    uint8_t i = ch - RF_CHANNEL_FIRST;
    if (on) b[i >> 3] |= (1U << (i & 7));
    else    b[i >> 3] &= ~(1U << (i & 7));
}

static inline uint8_t blCount(const uint8_t *b) {
    uint16_t n = 0;
    for (uint8_t i = 0; i < RF_CHANNEL_COUNT; i++) {
        if (b[i >> 3] & (1U << (i & 7))) n++;
    }
    return (uint8_t)n;
}

static inline uint32_t mix32(uint32_t x) {
    x += 0x9E3779B9UL;
    x = (x ^ (x >> 16)) * 0x85EBCA6BUL;
    x = (x ^ (x >> 13)) * 0xC2B2AE35UL;
    return x ^ (x >> 16);
}

// Deterministic adaptive pseudo-random channel calculation
static inline uint8_t hopChannel(uint32_t sf, uint32_t seed, const uint8_t *blacklist) {
    uint16_t healthy = RF_CHANNEL_COUNT - blCount(blacklist);
    if (healthy == 0) return RF_CHANNEL_FIRST;
    uint16_t rank = (uint16_t)(mix32(seed ^ sf) % healthy);
    for (uint8_t ch = RF_CHANNEL_FIRST; ch <= RF_CHANNEL_LAST; ++ch) {
        if (!blGet(blacklist, ch)) {
            if (rank == 0) return ch;
            rank--;
        }
    }
    return RF_CHANNEL_FIRST;
}

static inline uint8_t channel_for_hop(uint32_t sf, uint32_t seed, const uint8_t *blacklist) {
    return hopChannel(sf, seed, blacklist);
}

// ---------------- RF24 Safe Retuning & Fast Low-Latency I/O ----------------
static inline void setRadioChannel(RF24 &r, uint8_t ch) {
    r.setChannel(ch);
}

static inline bool txFrame(RF24 &radio, const void *frame) {
    radio.stopListening();
    radio.writeFast(frame, RF_PAYLOAD_SIZE, 1); // Fast SPI write to TX FIFO without multicast wait
    radio.txStandBy(1); // Fast wait until packet leaves antenna (~164us at 2Mbps)
    radio.startListening();
    return true;
}

static inline bool rxFrame(RF24 &radio, void *frame) {
    if (!radio.available()) return false;
    radio.read(frame, RF_PAYLOAD_SIZE);
    return true;
}

static inline bool radioCommonBegin(RF24 &radio) {
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    SPI.begin(RF_SCK_PIN, RF_MISO_PIN, RF_MOSI_PIN, RF_CSN_PIN);
    SPI.setFrequency(10000000); // 10 MHz Hardware SPI
#endif
    if (!radio.begin()) return false;
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_2MBPS); // 2 Mbps high-speed for ~164us air time & 625us slots
    radio.setChannel(RF_CHANNEL_SYNC);
    radio.setPayloadSize(RF_PAYLOAD_SIZE);
    radio.setAutoAck(false);
    radio.setCRCLength(RF24_CRC_16);
    radio.openWritingPipe(0xC3C3C3C3C3LL);
    radio.openReadingPipe(1, 0xC3C3C3C3C3LL);
    radio.startListening();
    return true;
}

// ---------------- RFC 8439 ChaCha20-Poly1305 Security ----------------
static inline uint32_t rotl32_local(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static inline void chacha20_core_block(uint32_t out[16], const uint32_t in[16]) {
    for (int i = 0; i < 16; i++) out[i] = in[i];
    for (int i = 0; i < 10; i++) {
        // Column rounds
        out[0] += out[4]; out[12] = rotl32_local(out[12] ^ out[0], 16);
        out[8] += out[12]; out[4] = rotl32_local(out[4] ^ out[8], 12);
        out[0] += out[4]; out[12] = rotl32_local(out[12] ^ out[0], 8);
        out[8] += out[12]; out[4] = rotl32_local(out[4] ^ out[8], 7);

        out[1] += out[5]; out[13] = rotl32_local(out[13] ^ out[1], 16);
        out[9] += out[13]; out[5] = rotl32_local(out[5] ^ out[9], 12);
        out[1] += out[5]; out[13] = rotl32_local(out[13] ^ out[1], 8);
        out[9] += out[13]; out[5] = rotl32_local(out[5] ^ out[9], 7);

        out[2] += out[6]; out[14] = rotl32_local(out[14] ^ out[2], 16);
        out[10] += out[14]; out[6] = rotl32_local(out[10] ^ out[6], 12);
        out[2] += out[6]; out[14] = rotl32_local(out[14] ^ out[2], 8);
        out[10] += out[14]; out[6] = rotl32_local(out[10] ^ out[6], 7);

        out[3] += out[7]; out[15] = rotl32_local(out[15] ^ out[3], 16);
        out[11] += out[15]; out[7] = rotl32_local(out[11] ^ out[7], 12);
        out[3] += out[7]; out[15] = rotl32_local(out[15] ^ out[3], 8);
        out[11] += out[15]; out[7] = rotl32_local(out[11] ^ out[7], 7);

        // Diagonal rounds
        out[0] += out[5]; out[15] = rotl32_local(out[15] ^ out[0], 16);
        out[10] += out[15]; out[5] = rotl32_local(out[10] ^ out[5], 12);
        out[0] += out[5]; out[15] = rotl32_local(out[15] ^ out[0], 8);
        out[10] += out[15]; out[5] = rotl32_local(out[10] ^ out[5], 7);

        out[1] += out[6]; out[12] = rotl32_local(out[12] ^ out[1], 16);
        out[11] += out[12]; out[6] = rotl32_local(out[11] ^ out[6], 12);
        out[1] += out[6]; out[12] = rotl32_local(out[12] ^ out[1], 8);
        out[11] += out[12]; out[6] = rotl32_local(out[11] ^ out[6], 7);

        out[2] += out[7]; out[13] = rotl32_local(out[13] ^ out[2], 16);
        out[8] += out[13]; out[7] = rotl32_local(out[8] ^ out[7], 12);
        out[2] += out[7]; out[13] = rotl32_local(out[13] ^ out[2], 8);
        out[8] += out[13]; out[7] = rotl32_local(out[8] ^ out[7], 7);

        out[3] += out[4]; out[14] = rotl32_local(out[14] ^ out[3], 16);
        out[9] += out[14]; out[4] = rotl32_local(out[9] ^ out[4], 12);
        out[3] += out[4]; out[14] = rotl32_local(out[14] ^ out[3], 8);
        out[9] += out[14]; out[4] = rotl32_local(out[9] ^ out[4], 7);
    }
    for (int i = 0; i < 16; i++) out[i] += in[i];
}

// Poly1305 Tag Generation
static inline void poly1305_compute(const uint8_t *msg, size_t msg_len, const uint8_t key[32], uint8_t tag[16]) {
    uint32_t r0, r1, r2, r3, r4;
    uint32_t s1, s2, s3, s4;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    r0 = (key[0] | (key[1]<<8) | (key[2]<<16) | (key[3]<<24)) & 0x3ffffff;
    r1 = ((key[3]>>2) | (key[4]<<6) | (key[5]<<14) | (key[6]<<22)) & 0x3ffff03;
    r2 = ((key[6]>>4) | (key[7]<<4) | (key[8]<<12) | (key[9]<<20)) & 0x3ffc0ff;
    r3 = ((key[9]>>6) | (key[10]<<2) | (key[11]<<10) | (key[12]<<18)) & 0x3f03fff;
    r4 = ((key[12]>>8) | (key[13]<<0) | (key[14]<<8) | (key[15]<<16)) & 0x00fffff;

    s1 = r1 * 5; s2 = r2 * 5; s3 = r3 * 5; s4 = r4 * 5;

    while (msg_len > 0) {
        uint64_t d0, d1, d2, d3, d4;
        uint8_t buf[16] = {0};
        size_t take = msg_len > 16 ? 16 : msg_len;
        memcpy(buf, msg, take);
        msg += take;
        msg_len -= take;

        h0 += (buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24)) & 0x3ffffff;
        h1 += ((buf[3]>>2) | (buf[4]<<6) | (buf[5]<<14) | (buf[6]<<22)) & 0x3ffffff;
        h2 += ((buf[6]>>4) | (buf[7]<<4) | (buf[8]<<12) | (buf[9]<<20)) & 0x3ffffff;
        h3 += ((buf[9]>>6) | (buf[10]<<2) | (buf[11]<<10) | (buf[12]<<18)) & 0x3ffffff;
        h4 += ((buf[12]>>8) | (buf[13]<<0) | (buf[14]<<8) | (buf[15]<<16)) | (1 << 24);

        d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    }

    uint32_t g0, g1, g2, g3, g4, c;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1 << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    uint64_t f0 = ((h0) | (h1 << 26)) + (key[16] | (key[17]<<8) | (key[18]<<16) | (key[19]<<24) | ((uint64_t)(key[20] | (key[21]<<8) | (key[22]<<16) | (key[23]<<24)) << 32));
    uint64_t f1 = ((h2) | (h3 << 26)) + (key[24] | (key[25]<<8) | (key[26]<<16) | (key[27]<<24) | ((uint64_t)(key[28] | (key[29]<<8) | (key[30]<<16) | (key[31]<<24)) << 32)) + (f0 < (key[16] | (key[17]<<8) | (key[18]<<16) | (key[19]<<24)));

    memcpy(tag, &f0, 8);
    memcpy(tag + 8, &f1, 8);
}

static inline void makeNonce(uint8_t nonce[12], uint8_t src, uint8_t dst,
                             uint32_t sf, uint16_t msgId, uint8_t frag) {
    nonce[0] = 0x53; nonce[1] = 0x50; nonce[2] = src; nonce[3] = dst;
    memcpy(&nonce[4], &sf, 4);
    memcpy(&nonce[8], &msgId, 2);
    nonce[10] = frag; nonce[11] = SP_VERSION;
}

static inline bool chachaEncrypt(const uint8_t *plain, uint8_t len,
                                 uint8_t *cipher, uint8_t tag[8],
                                 uint8_t src, uint8_t dst, uint32_t sf,
                                 uint16_t msgId, uint8_t frag) {
    if (len > DATA_PLAINTEXT_MAX) return false;
    uint8_t nonce[12];
    makeNonce(nonce, src, dst, sf, msgId, frag);

    // 1. ChaCha20 State setup with counter = 0 (Poly1305 subkey generation)
    uint32_t state[16];
    state[0] = 0x61707865; state[1] = 0x3320646e; state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) {
        state[4 + i] = E2E_KEY[i*4] | (E2E_KEY[i*4+1]<<8) | (E2E_KEY[i*4+2]<<16) | (E2E_KEY[i*4+3]<<24);
    }
    state[12] = 0; // counter = 0
    state[13] = nonce[0] | (nonce[1]<<8) | (nonce[2]<<16) | (nonce[3]<<24);
    state[14] = nonce[4] | (nonce[5]<<8) | (nonce[6]<<16) | (nonce[7]<<24);
    state[15] = nonce[8] | (nonce[9]<<8) | (nonce[10]<<16) | (nonce[11]<<24);

    uint32_t block0[16];
    chacha20_core_block(block0, state);
    uint8_t poly_key[32];
    memcpy(poly_key, block0, 32);

    // 2. ChaCha20 State setup with counter = 1 (Payload encryption)
    state[12] = 1; // counter = 1
    uint32_t block1[16];
    chacha20_core_block(block1, state);
    const uint8_t *ks = (const uint8_t*)block1;
    for (uint8_t i = 0; i < len; i++) {
        cipher[i] = plain[i] ^ ks[i];
    }

    // 3. Poly1305 MAC over ciphertext
    uint8_t full_tag[16];
    poly1305_compute(cipher, len, poly_key, full_tag);
    memcpy(tag, full_tag, 8); // 8-byte tag
    return true;
}

static inline bool chachaDecrypt(const uint8_t *cipher, uint8_t len,
                                 const uint8_t tag[8], uint8_t *plain,
                                 uint8_t src, uint8_t dst, uint32_t sf,
                                 uint16_t msgId, uint8_t frag) {
    if (len > DATA_PLAINTEXT_MAX) return false;
    uint8_t nonce[12];
    makeNonce(nonce, src, dst, sf, msgId, frag);

    // 1. ChaCha20 State setup with counter = 0 (Poly1305 subkey generation)
    uint32_t state[16];
    state[0] = 0x61707865; state[1] = 0x3320646e; state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) {
        state[4 + i] = E2E_KEY[i*4] | (E2E_KEY[i*4+1]<<8) | (E2E_KEY[i*4+2]<<16) | (E2E_KEY[i*4+3]<<24);
    }
    state[12] = 0; // counter = 0
    state[13] = nonce[0] | (nonce[1]<<8) | (nonce[2]<<16) | (nonce[3]<<24);
    state[14] = nonce[4] | (nonce[5]<<8) | (nonce[6]<<16) | (nonce[7]<<24);
    state[15] = nonce[8] | (nonce[9]<<8) | (nonce[10]<<16) | (nonce[11]<<24);

    uint32_t block0[16];
    chacha20_core_block(block0, state);
    uint8_t poly_key[32];
    memcpy(poly_key, block0, 32);

    // 2. Verify Poly1305 MAC over ciphertext in constant time
    uint8_t full_tag[16];
    poly1305_compute(cipher, len, poly_key, full_tag);
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (tag[i] ^ full_tag[i]);
    if (diff != 0) return false; // Authentication failure

    // 3. Decrypt payload
    state[12] = 1; // counter = 1
    uint32_t block1[16];
    chacha20_core_block(block1, state);
    const uint8_t *ks = (const uint8_t*)block1;
    for (uint8_t i = 0; i < len; i++) {
        plain[i] = cipher[i] ^ ks[i];
    }
    return true;
}

// Backwards compatibility wrappers
static inline bool gcmEncrypt(const uint8_t *plain, uint8_t len, uint8_t *cipher, uint8_t tag[8],
                              uint8_t src, uint8_t dst, uint32_t sf, uint16_t msgId, uint8_t frag) {
    return chachaEncrypt(plain, len, cipher, tag, src, dst, sf, msgId, frag);
}

static inline bool gcmDecrypt(const uint8_t *cipher, uint8_t len, const uint8_t tag[8], uint8_t *plain,
                              uint8_t src, uint8_t dst, uint32_t sf, uint16_t msgId, uint8_t frag) {
    return chachaDecrypt(cipher, len, tag, plain, src, dst, sf, msgId, frag);
}

static inline bool validHeader(uint16_t magic, uint8_t version, uint8_t type,
                               uint8_t src, uint8_t dst) {
    if (magic != SP_MAGIC || version != SP_VERSION) return false;
    if (type < FT_SYNC || type > FT_MAP) return false;
    if (src < NODE_A || src > NODE_C) return false;
    if (dst > NODE_C) return false;
    return true;
}

#endif // FHSS_H
