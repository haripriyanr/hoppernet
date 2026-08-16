#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "fhss.h"
#include "fhss_config.h"

// ---------------- Hardware & Role Config ----------------
#define ROLE            NODE_A
#define DST             NODE_B
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

// Sequence & stats
static uint8_t seq_counter = 0;
static volatile uint32_t stats_sent = 0;
static volatile uint32_t stats_acked = 0;
static volatile uint8_t  stats_current_ch = 0;
static volatile uint32_t stats_current_hop = 0;

// Outbound Message Queue (from Supabase)
struct OutboundMsg {
    char id[40];
    char text[PAYLOAD_LEN];
    uint8_t len;
};

static OutboundMsg msg_queue[QUEUE_SIZE];
static int q_head = 0;
static int q_tail = 0;
static int q_count = 0;
static portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

bool queue_push(const char *msg_id, const char *text, uint8_t len) {
    bool ok = false;
    portENTER_CRITICAL(&queueMux);
    if (q_count < QUEUE_SIZE) {
        memset(&msg_queue[q_head], 0, sizeof(OutboundMsg));
        strncpy(msg_queue[q_head].id, msg_id, 39);
        msg_queue[q_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memcpy(msg_queue[q_head].text, text, msg_queue[q_head].len);
        q_head = (q_head + 1) % QUEUE_SIZE;
        q_count++;
        ok = true;
    }
    portEXIT_CRITICAL(&queueMux);
    return ok;
}

bool queue_pop(OutboundMsg *out) {
    bool ok = false;
    portENTER_CRITICAL(&queueMux);
    if (q_count > 0) {
        *out = msg_queue[q_tail];
        q_tail = (q_tail + 1) % QUEUE_SIZE;
        q_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&queueMux);
    return ok;
}

static inline void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, FHSS_SEED, blacklist);
    if (ch != last_channel) {
        radio.setChannel(ch);
        last_channel = ch;
        stats_current_ch = ch;
    }
}

// ---------------- Background WiFi & Supabase Task (Core 0) ----------------
void networkTask(void *param) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOPPERNET_WIFI_SSID, HOPPERNET_WIFI_PASS);
    Serial.println("[NET] Connecting to WiFi: " HOPPERNET_WIFI_SSID);

    uint32_t last_poll_ms = 0;
    uint32_t last_telemetry_ms = 0;

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            // 1. Poll for pending messages from Supabase every 1 second
            if (millis() - last_poll_ms >= 1000) {
                last_poll_ms = millis();
                HTTPClient http;
                String url = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/messages?status=eq.pending&select=id,content&limit=5";
                http.begin(url);
                http.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                http.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);

                int code = http.GET();
                if (code == 200) {
                    String payload = http.getString();
                    // Simple parser for [{"id":"...","content":"..."}]
                    int idx = 0;
                    while ((idx = payload.indexOf("\"id\":\"", idx)) != -1) {
                        idx += 6;
                        int endId = payload.indexOf("\"", idx);
                        String idStr = payload.substring(idx, endId);

                        int cIdx = payload.indexOf("\"content\":\"", endId);
                        if (cIdx != -1) {
                            cIdx += 11;
                            int endContent = payload.indexOf("\"", cIdx);
                            String contentStr = payload.substring(cIdx, endContent);

                            if (queue_push(idStr.c_str(), contentStr.c_str(), contentStr.length())) {
                                // Mark as sent in Supabase
                                HTTPClient patchHttp;
                                String patchUrl = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/messages?id=eq." + idStr;
                                patchHttp.begin(patchUrl);
                                patchHttp.addHeader("Content-Type", "application/json");
                                patchHttp.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                                patchHttp.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);
                                patchHttp.PATCH("{\"status\":\"sent\",\"sent_at\":\"now()\"}");
                                patchHttp.end();
                            }
                            idx = endContent;
                        }
                    }
                }
                http.end();
            }

            // 2. Post telemetry every 3 seconds
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
                         "{\"node\":\"node_a\",\"sent\":%lu,\"acked\":%lu,\"received\":0,"
                         "\"buffer_depth\":0,\"blacklist_count\":%u,\"current_channel\":%u,"
                         "\"current_hop\":%lu,\"synced\":%s}",
                         (unsigned long)stats_sent, (unsigned long)stats_acked,
                         blacklist_count(blacklist), stats_current_ch,
                         (unsigned long)stats_current_hop, synced ? "true" : "false");

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
    Serial.println("  HopperNet Node A — Source (ESP32)       ");
    Serial.println("==========================================");

    blacklist_clear_all(blacklist);
    blacklist_clear_all(rx_blacklist);

    if (!radio.begin()) {
        Serial.println("[NODE_A] RF24 init FAILED — check wiring!");
        while (1) delay(100);
    }

    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);

    Serial.println("[NODE_A] Scanning channels for SYNC beacon...");
}

void loop() {
    // 1. Listen for SYNC or ACK from Node B
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
                    Serial.println("[NODE_A] *** SYNC ACQUIRED ***");
                }
            } else if (f.type == FRAME_TYPE_ACK && f.src == DST && f.dst == ROLE) {
                stats_acked++;
            }
        }
    }

    // If not synced, sweep channels hunting for SYNC
    if (!synced) {
        radio.setChannel(scan_ch);
        scan_ch = (scan_ch + 1) % (CHANNEL_BASE + NUM_CHANNELS);
        delay(2);
        return;
    }

    // Synchronized Hop Calculation
    uint32_t now_master = (uint32_t)((int32_t)micros() + clock_offset);
    uint32_t hop = now_master / DWELL_US;
    uint32_t phase = now_master % DWELL_US;
    stats_current_hop = hop;

    set_current_channel(hop);

    // 2. Transmit Window: A -> B during [2ms, 12ms)
    static uint32_t last_tx_hop = 0xFFFFFFFF;
    if (phase >= 2000 && phase < PHASE_A2B_US && hop != last_tx_hop) {
        last_tx_hop = hop;

        struct fhss_frame tx;
        memset(&tx, 0, sizeof(tx));
        tx.magic = FHSS_MAGIC;
        tx.type = FRAME_TYPE_DATA;
        tx.src = ROLE;
        tx.dst = DST;
        tx.seq = seq_counter++;
        tx.hop_index = (uint8_t)(hop & 0xFF);
        tx.flags = FLAG_ACK_REQ;

        OutboundMsg outMsg;
        if (queue_pop(&outMsg)) {
            tx.payload[0] = outMsg.len;
            memcpy(&tx.payload[1], outMsg.text, outMsg.len);
            Serial.printf("[NODE_A] TX User Msg #%u: \"%s\"\n", tx.seq, outMsg.text);
        } else {
            // Autonomous synthetic data
            char msg[24];
            snprintf(msg, sizeof(msg), "Vital#%u", tx.seq);
            size_t len = strlen(msg);
            tx.payload[0] = (uint8_t)len;
            memcpy(&tx.payload[1], msg, len);
        }

        frame_fill_crc(&tx, PAYLOAD_LEN);

        radio.stopListening();
        radio.write(&tx, MAX_FRAME_LEN);
        stats_sent++;
        radio.startListening();
    }
}
