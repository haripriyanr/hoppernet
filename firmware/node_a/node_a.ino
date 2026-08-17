// HopperNet Node A — Source Endpoint (ESP32)
// 100% Local & Cloudless: Direct USB Serial + Slotted FHSS Radio Mesh

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "fhss.h"

// ---------------- Hardware & Role Config ----------------
#define RF_CE_PIN       4
#define RF_CSN_PIN      5
#define ROLE            NODE_A
#define RELAY           NODE_B
#define PEER            NODE_C
#define BAUD            115200

#define QUEUE_SIZE      32

// ---------------- Node State ----------------
RF24 radio(RF_CE_PIN, RF_CSN_PIN);
static int32_t clock_offset = 0;
static uint8_t synced = 0;
static uint8_t blacklist[BLACKLIST_SIZE];
static uint8_t rx_blacklist[BLACKLIST_SIZE];
static uint32_t rx_hop_index = 0;
static uint32_t rx_master_ts = 0;
static uint8_t scan_ch = 0;
static uint8_t last_channel = 255;

// Stats
static uint8_t seq_counter = 0;
static uint32_t stats_sent = 0;
static uint32_t stats_acked = 0;
static uint32_t stats_received = 0;
static uint8_t  stats_current_ch = 0;
static uint32_t stats_current_hop = 0;

// Outbound Message Queue (from Serial to Node C)
struct OutboundMsg {
    char text[PAYLOAD_LEN];
    uint8_t len;
};
static OutboundMsg out_queue[QUEUE_SIZE];
static int oq_head = 0, oq_tail = 0, oq_count = 0;

bool out_push(const char *text, uint8_t len) {
    if (oq_count < QUEUE_SIZE) {
        memset(&out_queue[oq_head], 0, sizeof(OutboundMsg));
        out_queue[oq_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memcpy(out_queue[oq_head].text, text, out_queue[oq_head].len);
        oq_head = (oq_head + 1) % QUEUE_SIZE;
        oq_count++;
        return true;
    }
    return false;
}

bool out_pop(OutboundMsg *out) {
    if (oq_count > 0) {
        *out = out_queue[oq_tail];
        oq_tail = (oq_tail + 1) % QUEUE_SIZE;
        oq_count--;
        return true;
    }
    return false;
}

static inline void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, FHSS_SEED, blacklist);
    if (ch != last_channel) {
        radio.setChannel(ch);
        last_channel = ch;
        stats_current_ch = ch;
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(1000);
    Serial.println(F("=========================================="));
    Serial.println(F("  HopperNet NODE A — Source Endpoint      "));
    Serial.println(F("=========================================="));

    blacklist_clear_all(blacklist);
    blacklist_clear_all(rx_blacklist);

    if (!radio.begin()) {
        Serial.println(F("[NODE_A] RF24 init FAILED — check wiring!"));
        while (1) delay(100);
    }

    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    Serial.println(F("[NODE_A] Scanning channels for SYNC beacon..."));
}

// ---------------- Loop ----------------
void loop() {
    // 1. Read Commands / Messages from USB Serial (Local App)
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.startsWith("SEND:")) {
            String msg = input.substring(5);
            if (out_push(msg.c_str(), msg.length())) {
                Serial.print(F("[NODE_A] QUEUED FOR TX: \""));
                Serial.print(msg);
                Serial.println(F("\""));
            }
        }
    }

    // 2. Process Received Radio Frames (SYNC, ACKs, Return Data from Node B)
    if (radio.available()) {
        struct fhss_frame f;
        radio.read(&f, MAX_FRAME_LEN);
        if (frame_valid(&f, PAYLOAD_LEN)) {
            if (f.type == FRAME_TYPE_SYNC) {
                memcpy(&rx_hop_index, &f.payload[0], 4);
                memcpy(&rx_master_ts, &f.payload[4], 4);
                blacklist_copy(rx_blacklist, &f.payload[8]);

                clock_offset = (int32_t)rx_master_ts - (int32_t)micros();
                blacklist_copy(blacklist, rx_blacklist);

                if (!synced) {
                    synced = 1;
                    set_current_channel(rx_hop_index + 1);
                    Serial.println(F("[NODE_A] *** SYNC ACQUIRED ***"));
                }
            } else if (f.type == FRAME_TYPE_ACK && f.src == RELAY && f.dst == ROLE) {
                stats_acked++;
            } else if (f.type == FRAME_TYPE_DATA && f.dst == ROLE) {
                // Return packet from Node C via Relay
                stats_received++;
                uint8_t len = f.payload[0];
                char msg[25] = {0};
                if (len > PAYLOAD_LEN - 1) len = PAYLOAD_LEN - 1;
                memcpy(msg, &f.payload[1], len);

                Serial.print(F("[NODE_A] RECV RETURN hop="));
                Serial.print(f.hop_index);
                Serial.print(F(" seq="));
                Serial.print(f.seq);
                Serial.print(F(" data=\""));
                Serial.print(msg);
                Serial.println(F("\""));

                // Send immediate ACK to Relay
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = RELAY;
                ack.seq = f.seq;
                ack.hop_index = f.hop_index;
                ack.payload[0] = f.seq;
                ack.payload[1] = 0x0D;
                frame_fill_crc(&ack, PAYLOAD_LEN);

                radio.stopListening();
                radio.write(&ack, MAX_FRAME_LEN);
                radio.startListening();
            }
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
    uint32_t phase = now_master % DWELL_US;
    stats_current_hop = hop;

    set_current_channel(hop);

    // 3. Transmit Window: Forward Slot [2ms, 7ms)
    static uint32_t last_tx_hop = 0xFFFFFFFF;
    if (phase >= PHASE_SYNC_US && phase < 7000 && hop != last_tx_hop) {
        last_tx_hop = hop;

        OutboundMsg outMsg;
        if (out_pop(&outMsg)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = RELAY;
            tx.seq = seq_counter++;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = outMsg.len;
            memcpy(&tx.payload[1], outMsg.text, outMsg.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            stats_sent++;
            radio.startListening();

            Serial.print(F("[NODE_A] TX FORWARD seq="));
            Serial.print(tx.seq);
            Serial.print(F(" data=\""));
            Serial.print(outMsg.text);
            Serial.println(F("\""));
        }
    }
}
