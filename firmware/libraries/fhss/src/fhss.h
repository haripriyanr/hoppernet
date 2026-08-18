#ifndef FHSS_H
#define FHSS_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <mbedtls/gcm.h>
#endif

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

// ---------------- RF Spectrum Configuration ----------------
#define NUM_SYNC_ANCHORS  4
// Anchors live at 2.483-2.513 GHz, above all Wi-Fi 1/6/11 bands (ends ~2.473 GHz)
static const uint8_t SYNC_ANCHORS[NUM_SYNC_ANCHORS] = { 83, 93, 103, 113 };

static inline uint8_t getSyncChannel(uint32_t sf) {
    return SYNC_ANCHORS[sf & 0x03];
}

#define RF_CHANNEL_SYNC   0        // Primary rendezvous / recovery channel
#define RF_CHANNEL_FIRST  2        // Channels 2..125 = 124 data channels
#define RF_CHANNEL_COUNT  124
#define RF_CHANNEL_LAST   125
#define BLACKLIST_BYTES   16       // 128 bits, 124 used
#define RF_PAYLOAD_SIZE   32
#define DATA_PLAINTEXT_MAX 8       // 8 bytes plaintext payload per frame fragment
#define GCM_TAG_SIZE      8        // 8-byte AES-GCM authentication tag
#define DATA_FLAG_E2E      0x01    // Payload is end-to-end encrypted
#define DATA_FLAG_RECOVERED 0x02   // Relay delivered a fragment from backlog
#define DATA_FLAG_CRITICAL 0x04    // Application marks the message as critical
// Two independent PRNG sequences: A->B (FHSS_SEED_AB) and B->C / C->B
// (FHSS_SEED_BC). Each hop rides its own hopping domain.
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

// ---------------- 50 ms Slotted Superframe Timing ----------------
// Node B is master clock:
// 0 - 4 ms   : Sync & Blacklist Beacon on rotating anchor + Channel 0
// 4 - 16 ms  : A -> B Forward Path on hop channel AB (12 ms window)
// 16 - 28 ms : B -> C Forward Drain on hop channel BC (12 ms window)
// 28 - 40 ms : C -> B Return Path on hop channel BC (12 ms window)
// 40 - 48 ms : B -> A Return Drain on hop channel AB (8 ms window)
// 48 - 50 ms : Guard / RPD Jammer carrier probe (2 ms window)
#define SUPERFRAME_US     50000UL
#define HOPS_PER_SEC      20       // 20 superframes per second (1000ms / 50ms)
#define SLOT_SYNC_US      4000UL
#define SLOT_AB_RX_US     12000UL
#define SLOT_BC_TX_US     12000UL
#define SLOT_BC_RX_US     12000UL
#define SLOT_AB_TX_US     8000UL

#define AB_RX_START       SLOT_SYNC_US                     // 4,000 us
#define BC_TX_START       (AB_RX_START + SLOT_AB_RX_US)    // 16,000 us
#define BC_RX_START       (BC_TX_START + SLOT_BC_TX_US)    // 28,000 us
#define AB_TX_START       (BC_RX_START + SLOT_BC_RX_US)    // 40,000 us
#define GUARD_START       (AB_TX_START + SLOT_AB_TX_US)    // 48,000 us

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
    uint32_t masterUs;    // 4 (Master microsecond timestamp for sub-microsecond phase lock)
    uint8_t  reserved[14];// 14 -> Total: 32 bytes
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
    uint32_t masterUs;    // 4 -> Total: 32 bytes
}; // Exactly 32 bytes

static_assert(sizeof(DataFrame) == 32, "DataFrame must be exactly 32 bytes");
static_assert(sizeof(AckFrame) == 32, "AckFrame must be exactly 32 bytes");
static_assert(sizeof(SyncFrame) == 32, "SyncFrame must be exactly 32 bytes");

// ---------------- Demo E2E Encryption Key ----------------
// Node A and Node C use this key for AES-GCM. Node B never decrypts payloads.
static const uint8_t E2E_KEY[16] = {
    0x53, 0x50, 0x2D, 0x45, 0x32, 0x45, 0x2D, 0x44,
    0x45, 0x4D, 0x4F, 0x2D, 0x32, 0x30, 0x32, 0x36
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
    radio.setDataRate(RF24_2MBPS); // 2 Mbps high-speed for ~164us air time & ~625us handshake
    radio.setChannel(RF_CHANNEL_SYNC);
    radio.setPayloadSize(RF_PAYLOAD_SIZE);
    radio.setAutoAck(false);
    radio.setCRCLength(RF24_CRC_16);
    radio.openWritingPipe(0xC3C3C3C3C3LL);
    radio.openReadingPipe(1, 0xC3C3C3C3C3LL);
    radio.startListening();
    return true;
}

// ---------------- Application Security (AES-128-GCM) ----------------
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
static inline void makeNonce(uint8_t nonce[12], uint8_t src, uint8_t dst,
                             uint32_t sf, uint16_t msgId, uint8_t frag) {
    nonce[0] = 0x53; nonce[1] = 0x50; nonce[2] = src; nonce[3] = dst;
    memcpy(&nonce[4], &sf, 4);
    memcpy(&nonce[8], &msgId, 2);
    nonce[10] = frag; nonce[11] = SP_VERSION;
}

static inline bool gcmEncrypt(const uint8_t *plain, uint8_t len,
                              uint8_t *cipher, uint8_t tag[8],
                              uint8_t src, uint8_t dst, uint32_t sf,
                              uint16_t msgId, uint8_t frag) {
    if (len > DATA_PLAINTEXT_MAX) return false;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, E2E_KEY, 128) != 0) {
        mbedtls_gcm_free(&g);
        return false;
    }
    uint8_t nonce[12];
    makeNonce(nonce, src, dst, sf, msgId, frag);
    int rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, nonce, sizeof(nonce),
                                       nullptr, 0, plain, cipher, GCM_TAG_SIZE, tag);
    mbedtls_gcm_free(&g);
    return rc == 0;
}

static inline bool gcmDecrypt(const uint8_t *cipher, uint8_t len,
                              const uint8_t tag[8], uint8_t *plain,
                              uint8_t src, uint8_t dst, uint32_t sf,
                              uint16_t msgId, uint8_t frag) {
    if (len > DATA_PLAINTEXT_MAX) return false;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, E2E_KEY, 128) != 0) {
        mbedtls_gcm_free(&g);
        return false;
    }
    uint8_t nonce[12];
    makeNonce(nonce, src, dst, sf, msgId, frag);
    int rc = mbedtls_gcm_auth_decrypt(&g, len, nonce, sizeof(nonce), nullptr, 0,
                                      tag, GCM_TAG_SIZE, cipher, plain);
    mbedtls_gcm_free(&g);
    return rc == 0;
}
#endif

static inline bool validHeader(uint16_t magic, uint8_t version, uint8_t type,
                               uint8_t src, uint8_t dst) {
    if (magic != SP_MAGIC || version != SP_VERSION) return false;
    if (type < FT_SYNC || type > FT_MAP) return false;
    if (src < NODE_A || src > NODE_C) return false;
    if (dst > NODE_C) return false;
    return true;
}

#endif // FHSS_H
