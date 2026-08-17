// HopperNet Node B — Master Relay & Dual-Direction Edge Buffer (Arduino Due)
// Hardware: Arduino Due (SAM3X8E ARM @ 84MHz) + nRF24L01+ + 16x2 I2C LCD

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RF24.h>
#include "fhss.h"

// ---------------- Hardware & Pin Config (Arduino Due) ----------------
#define CE_PIN          9
#define CSN_PIN         10
#define ROLE            NODE_B
#define NODE_SRC        NODE_A
#define NODE_DST        NODE_C
#define BAUD            115200

#define BUFFER_MAX_ITEMS 128

// ---------------- LCD & Radio State ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);
bool lcd_available = false;

RF24 radio(CE_PIN, CSN_PIN);
static uint8_t blacklist[BLACKLIST_SIZE];
static uint32_t hop_counter = 0;
static uint32_t last_hop = 0xFFFFFFFF;
static uint8_t last_channel = 255;
static uint8_t jam_counts[NUM_CHANNELS];

// Stats
static uint32_t stats_fwd_delivered = 0;
static uint32_t stats_rev_delivered = 0;
static uint32_t stats_recv_total = 0;
static uint16_t stats_buffer_total = 0;
static uint8_t  stats_blacklist_count = 0;
static uint8_t  stats_current_ch = 0;
static uint32_t stats_current_hop = 0;
static uint32_t last_lcd_update_ms = 0;

// Deduplication
static uint8_t last_seen_seq_a[256] = {0};
static uint8_t last_seen_seq_c[256] = {0};

// In-Memory Edge Buffer Structure
struct BufferedPacket {
    uint8_t src;
    uint8_t dst;
    uint8_t seq;
    uint8_t len;
    char payload[PAYLOAD_LEN];
};

// Queue 1: Forward Buffer (A -> C)
static BufferedPacket fwd_queue[BUFFER_MAX_ITEMS];
static int fq_head = 0, fq_tail = 0, fq_count = 0;

// Queue 2: Reverse Buffer (C -> A)
static BufferedPacket rev_queue[BUFFER_MAX_ITEMS];
static int rq_head = 0, rq_tail = 0, rq_count = 0;

// ---------------- Buffer Operations ----------------
bool fwd_push(uint8_t src, uint8_t dst, uint8_t seq, const char *data, uint8_t len) {
    if (fq_count < BUFFER_MAX_ITEMS) {
        fwd_queue[fq_head].src = src;
        fwd_queue[fq_head].dst = dst;
        fwd_queue[fq_head].seq = seq;
        fwd_queue[fq_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memset(fwd_queue[fq_head].payload, 0, PAYLOAD_LEN);
        memcpy(fwd_queue[fq_head].payload, data, fwd_queue[fq_head].len);
        fq_head = (fq_head + 1) % BUFFER_MAX_ITEMS;
        fq_count++;
        stats_buffer_total = fq_count + rq_count;
        return true;
    }
    return false;
}

bool fwd_peek(BufferedPacket *out) {
    if (fq_count > 0) {
        *out = fwd_queue[fq_tail];
        return true;
    }
    return false;
}

void fwd_pop() {
    if (fq_count > 0) {
        fq_tail = (fq_tail + 1) % BUFFER_MAX_ITEMS;
        fq_count--;
        stats_buffer_total = fq_count + rq_count;
    }
}

bool rev_push(uint8_t src, uint8_t dst, uint8_t seq, const char *data, uint8_t len) {
    if (rq_count < BUFFER_MAX_ITEMS) {
        rev_queue[rq_head].src = src;
        rev_queue[rq_head].dst = dst;
        rev_queue[rq_head].seq = seq;
        rev_queue[rq_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memset(rev_queue[rq_head].payload, 0, PAYLOAD_LEN);
        memcpy(rev_queue[rq_head].payload, data, rev_queue[rq_head].len);
        rq_head = (rq_head + 1) % BUFFER_MAX_ITEMS;
        rq_count++;
        stats_buffer_total = fq_count + rq_count;
        return true;
    }
    return false;
}

bool rev_peek(BufferedPacket *out) {
    if (rq_count > 0) {
        *out = rev_queue[rq_tail];
        return true;
    }
    return false;
}

void rev_pop() {
    if (rq_count > 0) {
        rq_tail = (rq_tail + 1) % BUFFER_MAX_ITEMS;
        rq_count--;
        stats_buffer_total = fq_count + rq_count;
    }
}

// ---------------- Helper Functions ----------------
static inline void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, FHSS_SEED, blacklist);
    if (ch != last_channel) {
        radio.setChannel(ch);
        last_channel = ch;
        stats_current_ch = ch;
    }
}

void broadcast_sync(uint32_t hop, uint32_t master_ts) {
    struct fhss_frame f;
    memset(&f, 0, sizeof(f));
    f.magic = FHSS_MAGIC;
    f.type = FRAME_TYPE_SYNC;
    f.src = ROLE;
    f.dst = 0;
    f.seq = (uint8_t)(hop & 0xFF);
    f.hop_index = (uint8_t)(hop & 0xFF);

    memcpy(&f.payload[0], &hop, 4);
    memcpy(&f.payload[4], &master_ts, 4);
    blacklist_copy(&f.payload[8], blacklist);

    frame_fill_crc(&f, PAYLOAD_LEN);

    radio.stopListening();
    radio.write(&f, MAX_FRAME_LEN);
    radio.startListening();
}

void scan_jammer() {
    bool carrier = radio.testCarrier();
    uint8_t ch = radio.getChannel();
    if (ch >= CHANNEL_BASE && ch < CHANNEL_BASE + NUM_CHANNELS) {
        int idx = ch - CHANNEL_BASE;
        if (carrier) {
            jam_counts[idx]++;
            if (jam_counts[idx] >= 3 && !blacklist_get(blacklist, ch)) {
                blacklist_set(blacklist, ch);
                stats_blacklist_count = blacklist_count(blacklist);
                Serial.print(F("[NODE_B] JAMMER DETECTED -> Blacklisted channel "));
                Serial.print(ch);
                Serial.print(F(" (total: "));
                Serial.print(stats_blacklist_count);
                Serial.println(F(")"));
                jam_counts[idx] = 0;
            }
        } else {
            if (jam_counts[idx] > 0) jam_counts[idx]--;
        }
    }
}

void update_lcd() {
    if (!lcd_available) return;
    char line0[17];
    char line1[17];
    snprintf(line0, sizeof(line0), "CH:%-3u  HOP:%-5lu", stats_current_ch, (unsigned long)(stats_current_hop % 100000));
    snprintf(line1, sizeof(line1), "F:%u R:%u JAM:%-2u", fq_count, rq_count, stats_blacklist_count);

    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcd.setCursor(0, 1);
    lcd.print(line1);
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(1000);
    Serial.println(F("=========================================="));
    Serial.println(F(" HopperNet Node B — Bidirectional Relay   "));
    Serial.println(F("=========================================="));

    blacklist_clear_all(blacklist);
    memset(jam_counts, 0, sizeof(jam_counts));

    Wire.begin();
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print(F("MEDRELAY DUAL-B "));
    lcd.setCursor(0, 1);
    lcd.print(F("BOOTING MESH... "));
    lcd_available = true;
    delay(800);

    if (!radio.begin()) {
        Serial.println(F("[NODE_B] RF24 init FAILED — check wiring!"));
        if (lcd_available) {
            lcd.clear();
            lcd.print(F("RF24 INIT FAILED"));
        }
        while (1) delay(100);
    }

    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    Serial.println(F("[NODE_B] Mesh ready. Starting bidirectional clock..."));
    if (lcd_available) lcd.clear();
}

// ---------------- Loop ----------------
void loop() {
    uint32_t now_us = micros();
    uint32_t hop = now_us / DWELL_US;
    uint32_t phase = now_us % DWELL_US;
    stats_current_hop = hop;

    set_current_channel(hop);

    // 1. Dwell Start: Broadcast SYNC [0, 2ms)
    if (hop != last_hop) {
        last_hop = hop;
        broadcast_sync(hop, now_us);
    }

    // 2. FORWARD PATH: [2ms, 13ms)
    // A -> B Receive window [2ms, 7ms)
    if (phase >= 2000 && phase < 7000) {
        if (radio.available()) {
            struct fhss_frame rx;
            radio.read(&rx, MAX_FRAME_LEN);
            if (frame_valid(&rx, PAYLOAD_LEN) && rx.type == FRAME_TYPE_DATA && rx.src == NODE_SRC) {
                stats_recv_total++;
                uint8_t plen = rx.payload[0];
                char msg[25] = {0};
                if (plen > PAYLOAD_LEN - 1) plen = PAYLOAD_LEN - 1;
                memcpy(msg, &rx.payload[1], plen);

                if (last_seen_seq_a[rx.seq] == 0) {
                    last_seen_seq_a[rx.seq] = 1;
                    fwd_push(NODE_SRC, NODE_DST, rx.seq, msg, plen);
                    Serial.print(F("[NODE_B] RX FWD A#"));
                    Serial.print(rx.seq);
                    Serial.print(F(": \""));
                    Serial.print(msg);
                    Serial.print(F("\" (FwdBuf: "));
                    Serial.print(fq_count);
                    Serial.println(F(")"));
                }

                // Immediate ACK to A
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = NODE_SRC;
                ack.seq = rx.seq;
                ack.hop_index = (uint8_t)(hop & 0xFF);
                ack.payload[0] = rx.seq;
                frame_fill_crc(&ack, PAYLOAD_LEN);

                radio.stopListening();
                radio.write(&ack, MAX_FRAME_LEN);
                radio.startListening();
            }
        }
    }

    // B -> C Drain window [7ms, 13ms)
    if (phase >= 7000 && phase < PHASE_FWD_US) {
        BufferedPacket pkt;
        if (fwd_peek(&pkt)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = NODE_DST;
            tx.seq = pkt.seq;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = pkt.len;
            memcpy(&tx.payload[1], pkt.payload, pkt.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            radio.startListening();

            uint32_t wait_start = micros();
            bool got_ack = false;
            while (micros() - wait_start < 4000) {
                if (radio.available()) {
                    struct fhss_frame ack;
                    radio.read(&ack, MAX_FRAME_LEN);
                    if (frame_valid(&ack, PAYLOAD_LEN) && ack.type == FRAME_TYPE_ACK &&
                        ack.src == NODE_DST && ack.dst == ROLE && ack.payload[0] == pkt.seq) {
                        got_ack = true;
                        break;
                    }
                }
            }

            if (got_ack) {
                fwd_pop();
                stats_fwd_delivered++;
                Serial.print(F("[NODE_B] Delivered to C#"));
                Serial.print(pkt.seq);
                Serial.print(F(" OK (FwdBuf: "));
                Serial.print(fq_count);
                Serial.println(F(")"));
            }
        }
    }

    // 3. REVERSE PATH: [13ms, 24ms)
    // C -> B Receive window [13ms, 18ms)
    if (phase >= PHASE_FWD_US && phase < 18000) {
        if (radio.available()) {
            struct fhss_frame rx;
            radio.read(&rx, MAX_FRAME_LEN);
            if (frame_valid(&rx, PAYLOAD_LEN) && rx.type == FRAME_TYPE_DATA && rx.src == NODE_DST) {
                stats_recv_total++;
                uint8_t plen = rx.payload[0];
                char msg[25] = {0};
                if (plen > PAYLOAD_LEN - 1) plen = PAYLOAD_LEN - 1;
                memcpy(msg, &rx.payload[1], plen);

                if (last_seen_seq_c[rx.seq] == 0) {
                    last_seen_seq_c[rx.seq] = 1;
                    rev_push(NODE_DST, NODE_SRC, rx.seq, msg, plen);
                    Serial.print(F("[NODE_B] RX REV C#"));
                    Serial.print(rx.seq);
                    Serial.print(F(": \""));
                    Serial.print(msg);
                    Serial.print(F("\" (RevBuf: "));
                    Serial.print(rq_count);
                    Serial.println(F(")"));
                }

                // Immediate ACK to C
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = NODE_DST;
                ack.seq = rx.seq;
                ack.hop_index = (uint8_t)(hop & 0xFF);
                ack.payload[0] = rx.seq;
                frame_fill_crc(&ack, PAYLOAD_LEN);

                radio.stopListening();
                radio.write(&ack, MAX_FRAME_LEN);
                radio.startListening();
            }
        }
    }

    // B -> A Drain window [18ms, 24ms)
    if (phase >= 18000 && phase < PHASE_REV_US) {
        BufferedPacket pkt;
        if (rev_peek(&pkt)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = NODE_SRC;
            tx.seq = pkt.seq;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = pkt.len;
            memcpy(&tx.payload[1], pkt.payload, pkt.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            radio.startListening();

            uint32_t wait_start = micros();
            bool got_ack = false;
            while (micros() - wait_start < 4000) {
                if (radio.available()) {
                    struct fhss_frame ack;
                    radio.read(&ack, MAX_FRAME_LEN);
                    if (frame_valid(&ack, PAYLOAD_LEN) && ack.type == FRAME_TYPE_ACK &&
                        ack.src == NODE_SRC && ack.dst == ROLE && ack.payload[0] == pkt.seq) {
                        got_ack = true;
                        break;
                    }
                }
            }

            if (got_ack) {
                rev_pop();
                stats_rev_delivered++;
                Serial.print(F("[NODE_B] Returned to A#"));
                Serial.print(pkt.seq);
                Serial.print(F(" OK (RevBuf: "));
                Serial.print(rq_count);
                Serial.println(F(")"));
            }
        }

        // Tail of dwell: carrier scan
        scan_jammer();
    }

    // Non-blocking LCD refresh every 200 ms
    if (millis() - last_lcd_update_ms >= 200) {
        last_lcd_update_ms = millis();
        update_lcd();
    }
}
