#include <Arduino.h>
#include <RF24.h>
#include "fhss.h"

// ---------------- hardware config (ESP32) ----------------
#define CE_PIN   4
#define CSN_PIN  5
#define ROLE     NODE_A
#define DST      NODE_B
#define SEED     0xC0FFEE01u
#define BAUD     115200

#define PHASE_SYNC_US  2000UL
#define PHASE_A2B_US   12000UL

// ---------------- node state ----------------
RF24 radio(CE_PIN, CSN_PIN);
static int32_t clock_offset = 0;
static uint8_t synced = 0;
static uint8_t blacklist[BLACKLIST_SIZE];
static uint8_t rx_blacklist[BLACKLIST_SIZE];
static uint32_t rx_hop_index = 0;
static uint32_t rx_master_ts = 0;
static uint8_t seq_out = 0;
static uint16_t acked = 0;
static uint32_t sent = 0;
static uint32_t last_stat_ms = 0;
static uint8_t last_chan = 255;
static uint8_t scan_ch = 0;

static void set_current_channel(uint32_t hop);
static void listen_sync(void);

void logline(const char *msg) {
    Serial.printf("[%lu] %s\n", (unsigned long)(micros() / 1000), msg);
}

void setup() {
    Serial.begin(BAUD);
    delay(2000);
    blacklist_clear_all(blacklist);
    blacklist_clear_all(rx_blacklist);

    if (!radio.begin()) {
        logline("RF24 init FAILED");
        while (1) delay(100);
    }
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    logline("Node A starting, scanning for sync...");
}

void loop() {
    // --- continuous receive / decode (SYNC, ACK) ---
    if (radio.available()) {
        struct fhss_frame f;
        radio.read(&f, MAX_FRAME_LEN);
        if (!frame_valid(&f, PAYLOAD_LEN)) return;

        if (f.type == FRAME_TYPE_SYNC) {
            memcpy(&rx_hop_index, &f.payload[0], 4);
            memcpy(&rx_master_ts, &f.payload[4], 4);
            blacklist_copy(rx_blacklist, &f.payload[8]);
            if (!synced) {
                clock_offset = (int32_t)rx_master_ts - (int32_t)micros();
                blacklist_copy(blacklist, rx_blacklist);
                synced = 1;
                set_current_channel(rx_hop_index + 1);
                logline("SYNC acquired");
            } else {
                // refresh blacklist + re-align clock each SYNC
                clock_offset = (int32_t)rx_master_ts - (int32_t)micros();
                blacklist_copy(blacklist, rx_blacklist);
            }
        } else if (f.type == FRAME_TYPE_ACK && f.src == DST && f.dst == ROLE) {
            acked++;
        }
    }

    if (!synced) {
        // scan channels for the master's SYNC beacon
        radio.setChannel(scan_ch);
        scan_ch = (scan_ch + 1) % (CHANNEL_BASE + NUM_CHANNELS);
        delay(2);
        return;
    }

    uint32_t now_master = (uint32_t)((int32_t)micros() + clock_offset);
    uint32_t hop = now_master / DWELL_US;
    uint32_t phase = now_master % DWELL_US;
    set_current_channel(hop);

    // transmit window: A -> B during [PHASE_SYNC_US, PHASE_A2B_US)
    if (phase >= PHASE_SYNC_US && phase < PHASE_A2B_US) {
        struct fhss_frame f;
        memset(&f, 0, sizeof(f));
        f.magic = FHSS_MAGIC;
        f.type = FRAME_TYPE_DATA;
        f.src = ROLE;
        f.dst = DST;
        f.seq = seq_out;
        f.hop_index = (uint8_t)hop;
        f.flags = FLAG_ACK_REQ;
        char msg[24];
        snprintf(msg, sizeof(msg), "Hello#%u", seq_out);
        size_t len = strlen(msg);
        f.payload[0] = (uint8_t)len;
        memcpy(&f.payload[1], msg, len);
        frame_fill_crc(&f, PAYLOAD_LEN);

        radio.stopListening();
        radio.write(&f, MAX_FRAME_LEN);
        sent++;
        radio.startListening();
    }

    if (millis() - last_stat_ms >= 5000) {
        last_stat_ms = millis();
        Serial.printf("[STAT] sent=%lu acked=%u ch=%u hop=%lu sync=%u\n",
                      (unsigned long)sent, acked, last_chan, (unsigned long)hop, synced);
    }
}

void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, SEED, blacklist);
    if (ch != last_chan) {
        radio.setChannel(ch);
        last_chan = ch;
    }
}
