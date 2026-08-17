// HopperNet Node C — Destination Endpoint (ESP32)
// Bidirectional: Receives data from A (A -> B -> C) & Dispatches return messages (C -> B -> A)

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "fhss.h"
#include "fhss_config.h"

// ---------------- Hardware & Role Config ----------------
#define ROLE            NODE_C
#define RELAY           NODE_B
#define PEER            NODE_A
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
static uint8_t seq_counter_c = 0;
static volatile uint32_t stats_sent = 0;
static volatile uint32_t stats_acked = 0;
static volatile uint32_t stats_received = 0;
static volatile uint8_t  stats_current_ch = 0;
static volatile uint32_t stats_current_hop = 0;

// Inbound Received Queue (from A via Relay)
struct InboundMsg {
    uint8_t seq;
    uint8_t hop;
    uint8_t ch;
    char text[PAYLOAD_LEN];
};
static InboundMsg in_queue[QUEUE_SIZE];
static int iq_head = 0, iq_tail = 0, iq_count = 0;
static portMUX_TYPE iqMux = portMUX_INITIALIZER_UNLOCKED;

// Outbound Message Queue (from C -> A via Relay)
struct OutboundMsg {
    char id[40];
    char text[PAYLOAD_LEN];
    uint8_t len;
};
static OutboundMsg out_queue[QUEUE_SIZE];
static int oq_head = 0, oq_tail = 0, oq_count = 0;
static portMUX_TYPE oqMux = portMUX_INITIALIZER_UNLOCKED;

bool in_push(uint8_t seq, uint8_t hop, uint8_t ch, const char *text) {
    bool ok = false;
    portENTER_CRITICAL(&iqMux);
    if (iq_count < QUEUE_SIZE) {
        in_queue[iq_head].seq = seq;
        in_queue[iq_head].hop = hop;
        in_queue[iq_head].ch = ch;
        memset(in_queue[iq_head].text, 0, PAYLOAD_LEN);
        strncpy(in_queue[iq_head].text, text, PAYLOAD_LEN - 1);
        iq_head = (iq_head + 1) % QUEUE_SIZE;
        iq_count++;
        ok = true;
    }
    portEXIT_CRITICAL(&iqMux);
    return ok;
}

bool in_pop(InboundMsg *out) {
    bool ok = false;
    portENTER_CRITICAL(&iqMux);
    if (iq_count > 0) {
        *out = in_queue[iq_tail];
        iq_tail = (iq_tail + 1) % QUEUE_SIZE;
        iq_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&iqMux);
    return ok;
}

bool out_push(const char *msg_id, const char *text, uint8_t len) {
    bool ok = false;
    portENTER_CRITICAL(&oqMux);
    if (oq_count < QUEUE_SIZE) {
        memset(&out_queue[oq_head], 0, sizeof(OutboundMsg));
        strncpy(out_queue[oq_head].id, msg_id, 39);
        out_queue[oq_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memcpy(out_queue[oq_head].text, text, out_queue[oq_head].len);
        oq_head = (oq_head + 1) % QUEUE_SIZE;
        oq_count++;
        ok = true;
    }
    portEXIT_CRITICAL(&oqMux);
    return ok;
}

bool out_pop(OutboundMsg *out) {
    bool ok = false;
    portENTER_CRITICAL(&oqMux);
    if (oq_count > 0) {
        *out = out_queue[oq_tail];
        oq_tail = (oq_tail + 1) % QUEUE_SIZE;
        oq_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&oqMux);
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
            // 1. Drain Inbound Received Queue to Supabase received_messages table
            InboundMsg inMsg;
            while (in_pop(&inMsg)) {
                HTTPClient http;
                String url = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/received_messages";
                http.begin(url);
                http.addHeader("Content-Type", "application/json");
                http.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                http.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);

                char json[256];
                snprintf(json, sizeof(json),
                         "{\"seq\":%u,\"hop\":%u,\"channel\":%u,\"src\":1,\"dst\":3,\"content\":\"%s\"}",
                         inMsg.seq, inMsg.hop, inMsg.ch, inMsg.text);
                http.POST(json);
                http.end();

                // Update original outbound message to delivered
                HTTPClient patchHttp;
                String patchUrl = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/messages?content=eq." + inMsg.text;
                patchHttp.begin(patchUrl);
                patchHttp.addHeader("Content-Type", "application/json");
                patchHttp.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                patchHttp.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);
                patchHttp.PATCH("{\"status\":\"delivered\",\"delivered_at\":\"now()\"}");
                patchHttp.end();
            }

            // 2. Poll for pending return messages targeted from Node C -> A (target_node = 1)
            if (millis() - last_poll_ms >= 1000) {
                last_poll_ms = millis();
                HTTPClient http;
                String url = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/messages?status=eq.pending&target_node=eq.1&select=id,content&limit=5";
                http.begin(url);
                http.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                http.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);

                int code = http.GET();
                if (code == 200) {
                    String payload = http.getString();
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

                            if (out_push(idStr.c_str(), contentStr.c_str(), contentStr.length())) {
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

            // 3. Post Telemetry every 3 seconds
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
                         "{\"node\":\"node_c\",\"sent\":%lu,\"acked\":%lu,\"received\":%lu,"
                         "\"buffer_depth\":0,\"blacklist_count\":%u,\"current_channel\":%u,"
                         "\"current_hop\":%lu,\"synced\":%s}",
                         (unsigned long)stats_sent, (unsigned long)stats_acked, (unsigned long)stats_received,
                         blacklist_count(blacklist), stats_current_ch,
                         (unsigned long)stats_current_hop, synced ? "true" : "false");

                http.POST(json);
                http.end();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ---------------- Setup & Core 1 RF Loop ----------------
void setup() {
    Serial.begin(BAUD);
    delay(1000);
    Serial.println("==========================================");
    Serial.println("  HopperNet Node C — Destination (ESP32)  ");
    Serial.println("==========================================");

    blacklist_clear_all(blacklist);
    blacklist_clear_all(rx_blacklist);

    if (!radio.begin()) {
        Serial.println("[NODE_C] RF24 init FAILED — check wiring!");
        while (1) delay(100);
    }

    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);

    Serial.println("[NODE_C] Scanning channels for SYNC beacon...");
}

void loop() {
    // 1. Process Received Frames (SYNC, ACKs, and Forward DATA from Node B)
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
                    Serial.println("[NODE_C] *** SYNC ACQUIRED ***");
                }
            } else if (f.type == FRAME_TYPE_ACK && f.src == RELAY && f.dst == ROLE) {
                stats_acked++;
            } else if (f.type == FRAME_TYPE_DATA && f.dst == ROLE) {
                stats_received++;
                uint8_t len = f.payload[0];
                char msg[25] = {0};
                if (len > PAYLOAD_LEN - 1) len = PAYLOAD_LEN - 1;
                memcpy(msg, &f.payload[1], len);

                Serial.printf("[NODE_C] RECV FWD hop=%u seq=%u data=\"%s\"\n", f.hop_index, f.seq, msg);
                in_push(f.seq, f.hop_index, last_channel, msg);

                // Immediate ACK back to Node B
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

    // 2. Transmit Window: C -> B in Reverse Slot [PHASE_FWD_US, 18ms)
    static uint32_t last_tx_hop_c = 0xFFFFFFFF;
    if (phase >= PHASE_FWD_US && phase < 18000 && hop != last_tx_hop_c) {
        last_tx_hop_c = hop;

        struct fhss_frame tx;
        memset(&tx, 0, sizeof(tx));
        tx.magic = FHSS_MAGIC;
        tx.type = FRAME_TYPE_DATA;
        tx.src = ROLE;
        tx.dst = RELAY; // Handed to relay to buffer & return to A
        tx.seq = seq_counter_c++;
        tx.hop_index = (uint8_t)(hop & 0xFF);
        tx.flags = FLAG_ACK_REQ;

        OutboundMsg outMsg;
        if (out_pop(&outMsg)) {
            tx.payload[0] = outMsg.len;
            memcpy(&tx.payload[1], outMsg.text, outMsg.len);
            Serial.printf("[NODE_C] TX Return Msg #%u: \"%s\"\n", tx.seq, outMsg.text);
        } else {
            char msg[24];
            snprintf(msg, sizeof(msg), "C-Ack#%u", tx.seq);
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
