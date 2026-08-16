#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "fhss.h"
#include "fhss_config.h"

// ---------------- Hardware & Role Config ----------------
#define ROLE            NODE_C
#define DST             NODE_B
#define BAUD            115200

#define RECV_QUEUE_SIZE 32

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
static volatile uint32_t stats_received = 0;
static volatile uint8_t  stats_current_ch = 0;
static volatile uint32_t stats_current_hop = 0;

// Inbound Received Message Queue (for Supabase POST)
struct InboundMsg {
    uint8_t seq;
    uint8_t hop;
    uint8_t ch;
    char text[PAYLOAD_LEN];
};

static InboundMsg recv_queue[RECV_QUEUE_SIZE];
static int rq_head = 0;
static int rq_tail = 0;
static int rq_count = 0;
static portMUX_TYPE rqueueMux = portMUX_INITIALIZER_UNLOCKED;

bool rqueue_push(uint8_t seq, uint8_t hop, uint8_t ch, const char *text) {
    bool ok = false;
    portENTER_CRITICAL(&rqueueMux);
    if (rq_count < RECV_QUEUE_SIZE) {
        recv_queue[rq_head].seq = seq;
        recv_queue[rq_head].hop = hop;
        recv_queue[rq_head].ch = ch;
        memset(recv_queue[rq_head].text, 0, PAYLOAD_LEN);
        strncpy(recv_queue[rq_head].text, text, PAYLOAD_LEN - 1);
        rq_head = (rq_head + 1) % RECV_QUEUE_SIZE;
        rq_count++;
        ok = true;
    }
    portEXIT_CRITICAL(&rqueueMux);
    return ok;
}

bool rqueue_pop(InboundMsg *out) {
    bool ok = false;
    portENTER_CRITICAL(&rqueueMux);
    if (rq_count > 0) {
        *out = recv_queue[rq_tail];
        rq_tail = (rq_tail + 1) % RECV_QUEUE_SIZE;
        rq_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&rqueueMux);
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

    uint32_t last_telemetry_ms = 0;

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            // 1. Drain Inbound Queue to Supabase received_messages table
            InboundMsg msg;
            while (rqueue_pop(&msg)) {
                HTTPClient http;
                String url = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/received_messages";
                http.begin(url);
                http.addHeader("Content-Type", "application/json");
                http.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                http.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);

                char json[256];
                snprintf(json, sizeof(json),
                         "{\"seq\":%u,\"hop\":%u,\"channel\":%u,\"src\":1,\"dst\":3,\"content\":\"%s\"}",
                         msg.seq, msg.hop, msg.ch, msg.text);

                http.POST(json);
                http.end();

                // Also update any matching pending/sent message in messages table to delivered
                HTTPClient patchHttp;
                String patchUrl = String(HOPPERNET_SUPABASE_URL) + "/rest/v1/messages?content=eq." + msg.text;
                patchHttp.begin(patchUrl);
                patchHttp.addHeader("Content-Type", "application/json");
                patchHttp.addHeader("apikey", HOPPERNET_SUPABASE_KEY);
                patchHttp.addHeader("Authorization", String("Bearer ") + HOPPERNET_SUPABASE_KEY);
                patchHttp.PATCH("{\"status\":\"delivered\",\"delivered_at\":\"now()\"}");
                patchHttp.end();
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
                         "{\"node\":\"node_c\",\"sent\":0,\"acked\":0,\"received\":%lu,"
                         "\"buffer_depth\":0,\"blacklist_count\":%u,\"current_channel\":%u,"
                         "\"current_hop\":%lu,\"synced\":%s}",
                         (unsigned long)stats_received,
                         blacklist_count(blacklist), stats_current_ch,
                         (unsigned long)stats_current_hop, synced ? "true" : "false");

                http.POST(json);
                http.end();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
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
    // 1. Receive & Process Frames (SYNC, DATA from Node B)
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
            } else if (f.type == FRAME_TYPE_DATA && f.src == DST && f.dst == ROLE) {
                stats_received++;
                uint8_t len = f.payload[0];
                char msg[25] = {0};
                if (len > PAYLOAD_LEN - 1) len = PAYLOAD_LEN - 1;
                memcpy(msg, &f.payload[1], len);

                Serial.printf("[NODE_C] RECV hop=%u seq=%u ch=%u data=\"%s\"\n",
                              f.hop_index, f.seq, last_channel, msg);

                // Queue for Supabase upload
                rqueue_push(f.seq, f.hop_index, last_channel, msg);

                // Send immediate ACK to Node B
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = DST;
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

    // Sweep scan if unsynced
    if (!synced) {
        radio.setChannel(scan_ch);
        scan_ch = (scan_ch + 1) % (CHANNEL_BASE + NUM_CHANNELS);
        delay(2);
        return;
    }

    // Synchronized channel hop
    uint32_t now_master = (uint32_t)((int32_t)micros() + clock_offset);
    uint32_t hop = now_master / DWELL_US;
    stats_current_hop = hop;
    set_current_channel(hop);
}
