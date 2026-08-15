#include <Arduino.h>
#include <stdarg.h>
#include <RF24.h>
#include "fhss.h"

// ---------------- hardware config (Arduino Due) ----------------
#define CE_PIN   9
#define CSN_PIN  10
#define ROLE     NODE_C
#define DST      NODE_B
#define SEED     0xC0FFEE01u
#define BAUD     115200

#define PHASE_SYNC_US  2000UL
#define PHASE_B2C_US   12000UL

// ---------------- node state ----------------
RF24 radio(CE_PIN, CSN_PIN);
static int32_t clock_offset = 0;
static uint8_t synced = 0;
static uint8_t blacklist[BLACKLIST_SIZE];
static uint8_t rx_blacklist[BLACKLIST_SIZE];
static uint32_t rx_hop_index = 0;
static uint32_t rx_master_ts = 0;
static uint32_t received = 0;
static uint32_t last_seen_ms = 0;
static uint8_t last_chan = 255;
static uint8_t scan_ch = 0;

static void set_current_channel(uint32_t hop);

void serial_printf(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

void logline(const char *msg) {
    serial_printf("[%lu] %s\n", (unsigned long)(micros() / 1000), msg);
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

    logline("Node C starting, scanning for sync...");
}

void loop() {
    // --- continuous receive / decode (SYNC, DATA) ---
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
                clock_offset = (int32_t)rx_master_ts - (int32_t)micros();
                blacklist_copy(blacklist, rx_blacklist);
            }
        } else if (f.type == FRAME_TYPE_DATA && f.src == DST && f.dst == ROLE) {
            received++;
            uint8_t len = f.payload[0];
            char msg[25];
            if (len > PAYLOAD_LEN - 1) len = PAYLOAD_LEN - 1;
            memcpy(msg, &f.payload[1], len);
            msg[len] = 0;
            serial_printf("[RECV] hop=%u seq=%u data=\"%s\"\n", f.hop_index, f.seq, msg);            last_seen_ms = millis();

            struct fhss_frame ack;
            memset(&ack, 0, sizeof(ack));
            ack.magic = FHSS_MAGIC;
            ack.type = FRAME_TYPE_ACK;
            ack.src = ROLE;
            ack.dst = DST;
            ack.seq = 0;
            ack.hop_index = f.hop_index;
            ack.payload[0] = f.seq;
            ack.payload[1] = 0x0D;
            frame_fill_crc(&ack, PAYLOAD_LEN);
            radio.stopListening();
            radio.write(&ack, MAX_FRAME_LEN);
            radio.startListening();
        }
    }

    if (!synced) {
        radio.setChannel(scan_ch);
        scan_ch = (scan_ch + 1) % (CHANNEL_BASE + NUM_CHANNELS);
        delay(2);
        return;
    }

    uint32_t now_master = (uint32_t)((int32_t)micros() + clock_offset);
    uint32_t hop = now_master / DWELL_US;
    set_current_channel(hop);

    if (millis() - last_seen_ms > 4000) {
        last_seen_ms = millis();
        serial_printf("[STAT] received=%lu ch=%u hop=%lu sync=%u\n",
                      (unsigned long)received, last_chan, (unsigned long)hop, synced);
    }
}

void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, SEED, blacklist);
    if (ch != last_chan) {
        radio.setChannel(ch);
        last_chan = ch;
    }
}
