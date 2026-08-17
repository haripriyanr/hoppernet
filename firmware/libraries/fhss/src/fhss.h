#ifndef FHSS_H
#define FHSS_H

#include <stdint.h>
#include <stddef.h>

#define FHSS_MAGIC        0x5A
#define NODE_A            1
#define NODE_B            2
#define NODE_C            3

#define FHSS_SEED         0xC0FFEE01u
static const uint8_t FHSS_PIPE_ADDR[5] = {0x48, 0x4F, 0x50, 0x50, 0x31}; // "HOPP1"

#define FRAME_TYPE_SYNC       0x01
#define FRAME_TYPE_DATA       0x02
#define FRAME_TYPE_ACK        0x03
#define FRAME_TYPE_JAMREPORT  0x04
#define FRAME_TYPE_STATUS     0x05

#define NUM_CHANNELS      124
#define CHANNEL_BASE      2
#define DWELL_US          25000UL
#define PHASE_SYNC_US     2000UL
#define PHASE_FWD_US      13000UL   // [2ms, 13ms): Forward Path (A -> B -> C)
#define PHASE_REV_US      24000UL   // [13ms, 24ms): Reverse Path (C -> B -> A)
#define SYNC_PERIOD_HOPS  20

#define HEADER_LEN        8
#define PAYLOAD_LEN       24
#define MAX_FRAME_LEN     (HEADER_LEN + PAYLOAD_LEN)

#define FLAG_ACK_REQ      0x01

#define BLACKLIST_SIZE    ((NUM_CHANNELS + 7) / 8)

struct fhss_telemetry {
    uint32_t sent;
    uint32_t acked;
    uint32_t received;
    uint16_t buffer_depth;
    uint8_t  blacklist_count;
    uint8_t  current_channel;
    uint32_t current_hop;
    uint8_t  synced;
};

struct fhss_frame {
    uint8_t magic;
    uint8_t type;
    uint8_t src;
    uint8_t dst;
    uint8_t seq;
    uint8_t hop_index;
    uint8_t flags;
    uint8_t crc;
    uint8_t payload[PAYLOAD_LEN];
} __attribute__((packed));

static inline uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ 0x07);
            else crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static inline uint8_t frame_crc(const struct fhss_frame *f, size_t payload_len) {
    return crc8(f->payload, payload_len);
}

static inline void frame_fill_crc(struct fhss_frame *f, size_t payload_len) {
    f->crc = frame_crc(f, payload_len);
}

static inline uint8_t frame_valid(const struct fhss_frame *f, size_t payload_len) {
    if (f->magic != FHSS_MAGIC) return 0;
    if (frame_crc(f, payload_len) != f->crc) return 0;
    return 1;
}

static inline uint32_t xorshift32(uint32_t state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static inline void blacklist_set(uint8_t *mask, int ch) {
    if (ch >= CHANNEL_BASE && ch < CHANNEL_BASE + NUM_CHANNELS) {
        int idx = ch - CHANNEL_BASE;
        mask[idx >> 3] |= (uint8_t)(1 << (idx & 7));
    }
}

static inline void blacklist_clear(uint8_t *mask, int ch) {
    if (ch >= CHANNEL_BASE && ch < CHANNEL_BASE + NUM_CHANNELS) {
        int idx = ch - CHANNEL_BASE;
        mask[idx >> 3] &= (uint8_t)~(1 << (idx & 7));
    }
}

static inline uint8_t blacklist_get(const uint8_t *mask, int ch) {
    if (ch < CHANNEL_BASE || ch >= CHANNEL_BASE + NUM_CHANNELS) return 0;
    int idx = ch - CHANNEL_BASE;
    return (mask[idx >> 3] >> (idx & 7)) & 1;
}

static inline uint32_t blacklist_count(const uint8_t *mask) {
    uint32_t n = 0;
    for (int i = 0; i < BLACKLIST_SIZE; i++) {
        uint8_t v = mask[i];
        while (v) { n += (v & 1); v >>= 1; }
    }
    return n;
}

static inline void blacklist_clear_all(uint8_t *mask) {
    for (int i = 0; i < BLACKLIST_SIZE; i++) mask[i] = 0;
}

static inline void blacklist_copy(uint8_t *dst, const uint8_t *src) {
    for (int i = 0; i < BLACKLIST_SIZE; i++) dst[i] = src[i];
}

static inline uint8_t blacklist_equal(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < BLACKLIST_SIZE; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Deterministic channel selection: identical algorithm on every node keeps
 * A / B / C hopping in lockstep even when channels are blacklisted. */
static inline uint8_t channel_for_hop(uint32_t hop, uint32_t seed,
                                      const uint8_t *blacklist) {
    uint32_t state = seed ^ (hop * 2654435761u);
    for (int attempt = 0; attempt < NUM_CHANNELS * 2; attempt++) {
        state = xorshift32(state);
        uint8_t ch = (uint8_t)(CHANNEL_BASE + (state % NUM_CHANNELS));
        if (!blacklist_get(blacklist, ch)) return ch;
    }
    return (uint8_t)(CHANNEL_BASE + (hop % NUM_CHANNELS));
}

#endif
