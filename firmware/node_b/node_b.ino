// HopperNet Node B — Master Relay & Store-and-Forward Edge Buffer (Arduino Due)
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
#define DST_A           NODE_A
#define DST_C           NODE_C
#define BAUD            115200

#define BUFFER_MAX_ITEMS 256  // 256 packets stored in Due's 96 KB SRAM (~6.6 KB)

// ---------------- LCD & Radio State ----------------
// Standard I2C LCD on Due pins 20 (SDA) / 21 (SCL). Default address 0x27 (or 0x3F).
LiquidCrystal_I2C lcd(0x27, 16, 2);
bool lcd_available = false;

RF24 radio(CE_PIN, CSN_PIN);
static uint8_t blacklist[BLACKLIST_SIZE];
static uint32_t hop_counter = 0;
static uint32_t last_hop = 0xFFFFFFFF;
static uint8_t last_channel = 255;
static uint8_t jam_counts[NUM_CHANNELS];

// Stats
static uint32_t stats_sent_c = 0;
static uint32_t stats_acked_c = 0;
static uint32_t stats_recv_a = 0;
static uint16_t stats_buffer_depth = 0;
static uint8_t  stats_blacklist_count = 0;
static uint8_t  stats_current_ch = 0;
static uint32_t stats_current_hop = 0;
static uint32_t last_lcd_update_ms = 0;

// Deduplication
static uint8_t last_seen_seq_a[256] = {0};

// In-Memory Edge Buffer structure
struct BufferedPacket {
    uint8_t seq;
    uint8_t len;
    char payload[PAYLOAD_LEN];
};

static BufferedPacket ring_buffer[BUFFER_MAX_ITEMS];
static int buf_head = 0;
static int buf_tail = 0;
static int buf_count = 0;

// ---------------- Buffer Operations (SRAM Ring Buffer) ----------------
bool buffer_push(uint8_t seq, const char *data, uint8_t len) {
    if (buf_count < BUFFER_MAX_ITEMS) {
        ring_buffer[buf_head].seq = seq;
        ring_buffer[buf_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memset(ring_buffer[buf_head].payload, 0, PAYLOAD_LEN);
        memcpy(ring_buffer[buf_head].payload, data, ring_buffer[buf_head].len);
        buf_head = (buf_head + 1) % BUFFER_MAX_ITEMS;
        buf_count++;
        stats_buffer_depth = buf_count;
        return true;
    }
    return false;
}

bool buffer_peek(BufferedPacket *out) {
    if (buf_count > 0) {
        *out = ring_buffer[buf_tail];
        return true;
    }
    return false;
}

void buffer_pop() {
    if (buf_count > 0) {
        buf_tail = (buf_tail + 1) % BUFFER_MAX_ITEMS;
        buf_count--;
        stats_buffer_depth = buf_count;
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
    f.dst = 0; // Broadcast
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
    snprintf(line1, sizeof(line1), "BUF:%-2u pk JAM:%-2u", stats_buffer_depth, stats_blacklist_count);

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
    Serial.println(F("  HopperNet Node B — Master Relay (Due)   "));
    Serial.println(F("=========================================="));

    blacklist_clear_all(blacklist);
    memset(jam_counts, 0, sizeof(jam_counts));

    // Initialize 16x2 LCD
    Wire.begin();
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print(F("MEDRELAY NODE B "));
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

    Serial.println(F("[NODE_B] Mesh ready. Starting master FHSS clock..."));
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

    // 2. A -> B RX Window [2ms, 12ms)
    if (phase >= 2000 && phase < PHASE_A2B_US) {
        if (radio.available()) {
            struct fhss_frame rx;
            radio.read(&rx, MAX_FRAME_LEN);
            if (frame_valid(&rx, PAYLOAD_LEN) && rx.type == FRAME_TYPE_DATA && rx.src == DST_A) {
                stats_recv_a++;
                uint8_t plen = rx.payload[0];
                char msg[25] = {0};
                if (plen > PAYLOAD_LEN - 1) plen = PAYLOAD_LEN - 1;
                memcpy(msg, &rx.payload[1], plen);

                // Deduplicate & buffer
                if (last_seen_seq_a[rx.seq] == 0) {
                    last_seen_seq_a[rx.seq] = 1;
                    buffer_push(rx.seq, msg, plen);
                    Serial.print(F("[NODE_B] RX A#"));
                    Serial.print(rx.seq);
                    Serial.print(F(": \""));
                    Serial.print(msg);
                    Serial.print(F("\" (Buffer: "));
                    Serial.print(stats_buffer_depth);
                    Serial.println(F(")"));
                }

                // Send ACK immediately
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = DST_A;
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

    // 3. B -> C TX Window & Store-and-Forward Drain [12ms, 25ms)
    if (phase >= PHASE_A2B_US) {
        BufferedPacket pkt;
        if (buffer_peek(&pkt)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = DST_C;
            tx.seq = pkt.seq;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = pkt.len;
            memcpy(&tx.payload[1], pkt.payload, pkt.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            stats_sent_c++;
            radio.startListening();

            // Wait up to 6ms for Node C ACK
            uint32_t wait_start = micros();
            bool got_ack = false;
            while (micros() - wait_start < 6000) {
                if (radio.available()) {
                    struct fhss_frame ack;
                    radio.read(&ack, MAX_FRAME_LEN);
                    if (frame_valid(&ack, PAYLOAD_LEN) && ack.type == FRAME_TYPE_ACK &&
                        ack.src == DST_C && ack.dst == ROLE && ack.payload[0] == pkt.seq) {
                        got_ack = true;
                        break;
                    }
                }
            }

            if (got_ack) {
                buffer_pop();
                stats_acked_c++;
                Serial.print(F("[NODE_B] Delivered C#"));
                Serial.print(pkt.seq);
                Serial.print(F(" OK (Buffer: "));
                Serial.print(stats_buffer_depth);
                Serial.println(F(")"));
            }
        }

        // Tail of hop: Jammer scan
        scan_jammer();
    }

    // Non-blocking LCD refresh every 200 ms
    if (millis() - last_lcd_update_ms >= 200) {
        last_lcd_update_ms = millis();
        update_lcd();
    }
}
