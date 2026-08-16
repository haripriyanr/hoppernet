#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include "fhss.h"
#include "fhss_config.h"

// ---------------- Hardware & Role Config ----------------
#define ROLE            NODE_B
#define DST_A           NODE_A
#define DST_C           NODE_C
#define BAUD            115200

#define BUFFER_MAX_ITEMS 128
#define BUFFER_FILE      "/edge_buffer.dat"

// ---------------- Node State ----------------
RF24 radio(RF_CE_PIN, RF_CSN_PIN);
static uint8_t blacklist[BLACKLIST_SIZE];
static uint32_t hop_counter = 0;
static uint32_t last_hop = 0xFFFFFFFF;
static uint8_t last_channel = 255;
static uint8_t jam_counts[NUM_CHANNELS];

// Stats
static volatile uint32_t stats_sent_c = 0;
static volatile uint32_t stats_acked_c = 0;
static volatile uint32_t stats_recv_a = 0;
static volatile uint16_t stats_buffer_depth = 0;
static volatile uint8_t  stats_blacklist_count = 0;
static volatile uint8_t  stats_current_ch = 0;
static volatile uint32_t stats_current_hop = 0;

// Deduplication
static uint8_t last_seen_seq_a[256] = {0};

// In-memory + Persistent Edge Buffer structure
struct BufferedPacket {
    uint8_t seq;
    uint8_t len;
    char payload[PAYLOAD_LEN];
};

static BufferedPacket ring_buffer[BUFFER_MAX_ITEMS];
static int buf_head = 0;
static int buf_tail = 0;
static int buf_count = 0;
static portMUX_TYPE bufferMux = portMUX_INITIALIZER_UNLOCKED;

// Blacklist notification queue
static int pending_blacklist_ch = -1;

// ---------------- Buffer Operations (RAM + SPIFFS) ----------------
void save_buffer_to_spiffs() {
    File f = SPIFFS.open(BUFFER_FILE, FILE_WRITE);
    if (!f) return;
    f.write((uint8_t*)&buf_count, sizeof(buf_count));
    for (int i = 0; i < buf_count; i++) {
        int idx = (buf_tail + i) % BUFFER_MAX_ITEMS;
        f.write((uint8_t*)&ring_buffer[idx], sizeof(BufferedPacket));
    }
    f.close();
}

void load_buffer_from_spiffs() {
    if (!SPIFFS.exists(BUFFER_FILE)) return;
    File f = SPIFFS.open(BUFFER_FILE, FILE_READ);
    if (!f) return;
    if (f.read((uint8_t*)&buf_count, sizeof(buf_count)) == sizeof(buf_count)) {
        if (buf_count > BUFFER_MAX_ITEMS) buf_count = BUFFER_MAX_ITEMS;
        buf_tail = 0;
        buf_head = buf_count % BUFFER_MAX_ITEMS;
        for (int i = 0; i < buf_count; i++) {
            f.read((uint8_t*)&ring_buffer[i], sizeof(BufferedPacket));
        }
        Serial.printf("[NODE_B] Restored %d packets from persistent SPIFFS storage\n", buf_count);
    }
    f.close();
}

bool buffer_push(uint8_t seq, const char *data, uint8_t len) {
    bool ok = false;
    portENTER_CRITICAL(&bufferMux);
    if (buf_count < BUFFER_MAX_ITEMS) {
        ring_buffer[buf_head].seq = seq;
        ring_buffer[buf_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memset(ring_buffer[buf_head].payload, 0, PAYLOAD_LEN);
        memcpy(ring_buffer[buf_head].payload, data, ring_buffer[buf_head].len);
        buf_head = (buf_head + 1) % BUFFER_MAX_ITEMS;
        buf_count++;
        stats_buffer_depth = buf_count;
        ok = true;
    }
    portEXIT_CRITICAL(&bufferMux);
    if (ok) save_buffer_to_spiffs();
    return ok;
}

bool buffer_peek(BufferedPacket *out) {
    bool ok = false;
    portENTER_CRITICAL(&bufferMux);
    if (buf_count > 0) {
        *out = ring_buffer[buf_tail];
        ok = true;
    }
    portEXIT_CRITICAL(&bufferMux);
    return ok;
}

void buffer_pop() {
    portENTER_CRITICAL(&bufferMux);
    if (buf_count > 0) {
        buf_tail = (buf_tail + 1) % BUFFER_MAX_ITEMS;
        buf_count--;
        stats_buffer_depth = buf_count;
    }
    portEXIT_CRITICAL(&bufferMux);
    save_buffer_to_spiffs();
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
                pending_blacklist_ch = ch;
                Serial.printf("[NODE_B] JAMMER DETECTED -> Blacklisted channel %u (total: %u)\n", 
                              ch, stats_blacklist_count);
                jam_counts[idx] = 0;
            }
        } else {
            if (jam_counts[idx] > 0) jam_counts[idx]--;
        }
    }
}

// ---------------- Background WiFi & Supabase Task (Core 0) ----------------
void networkTask(void *param) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOPPERNET_WIFI_SSID, HOPPERNET_WIFI_PASS);
    Serial.println("[NET] Connecting to WiFi: " HOPPERNET_WIFI_SSID);

    uint32_t last_telemetry_ms = 0;
    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            // Report Telemetry every 3 seconds
            if (millis() - last_telemetry_ms >= 3000) {
                last_telemetry_ms = millis();
                HTTPClient http;
                String url = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/telemetry";
                http.begin(url);
                http.addHeader("Content-Type", "application/json");
                http.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                http.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);

                char json[256];
                snprintf(json, sizeof(json),
                         "{\"node\":\"node_b\",\"sent\":%lu,\"acked\":%lu,\"received\":%lu,"
                         "\"buffer_depth\":%u,\"blacklist_count\":%u,\"current_channel\":%u,"
                         "\"current_hop\":%lu,\"synced\":true}",
                         (unsigned long)stats_sent_c, (unsigned long)stats_acked_c,
                         (unsigned long)stats_recv_a, stats_buffer_depth,
                         stats_blacklist_count, stats_current_ch, (unsigned long)stats_current_hop);

                int code = http.POST(json);
                http.end();
            }

            // Report Blacklist Events
            if (pending_blacklist_ch > 0) {
                int ch = pending_blacklist_ch;
                pending_blacklist_ch = -1;
                HTTPClient http;
                String url = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/blacklist_events";
                http.begin(url);
                http.addHeader("Content-Type", "application/json");
                http.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                http.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);

                char json[160];
                snprintf(json, sizeof(json),
                         "{\"channel\":%d,\"action\":\"blacklisted\",\"reason\":\"RPD carrier threshold exceeded\"}",
                         ch);
                http.POST(json);
                http.end();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------- Setup & Core 1 RF Loop ----------------
void setup() {
    Serial.begin(BAUD);
    delay(1000);
    Serial.println("==========================================");
    Serial.println("  HopperNet Node B — Relay & Master Clock ");
    Serial.println("==========================================");

    blacklist_clear_all(blacklist);
    memset(jam_counts, 0, sizeof(jam_counts));

    if (!SPIFFS.begin(true)) {
        Serial.println("[NODE_B] SPIFFS mount failed!");
    } else {
        load_buffer_from_spiffs();
    }

    if (!radio.begin()) {
        Serial.println("[NODE_B] RF24 init FAILED — check wiring!");
        while (1) delay(100);
    }

    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    // Launch Core 0 Network Task
    xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);

    Serial.println("[NODE_B] Mesh ready. Starting master FHSS clock...");
}

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
                    Serial.printf("[NODE_B] RX A#%u: \"%s\" (Buffer: %d)\n", rx.seq, msg, stats_buffer_depth);
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
                Serial.printf("[NODE_B] Delivered C#%u OK (Buffer: %d)\n", pkt.seq, stats_buffer_depth);
            }
        }

        // Tail of hop: Jammer scan
        scan_jammer();
    }
}
