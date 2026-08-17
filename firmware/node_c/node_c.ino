// HopperNet Node C — Destination Endpoint (ESP32)
// Dual-Mode: SoftAP ("hopperc") + USB Serial + Slotted FHSS Radio Mesh

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WebServer.h>
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
WebServer server(80);

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
static uint32_t stats_sent = 0;
static uint32_t stats_acked = 0;
static uint32_t stats_received = 0;
static uint8_t  stats_current_ch = 0;
static uint32_t stats_current_hop = 0;

// Outbound Message Queue (from Web/Serial to Node A)
struct OutboundMsg {
    char text[PAYLOAD_LEN];
    uint8_t len;
};
static OutboundMsg out_queue[QUEUE_SIZE];
static int oq_head = 0, oq_tail = 0, oq_count = 0;

// Inbound Message History (Received from Node A)
struct InboundMsg {
    uint8_t seq;
    uint8_t hop;
    char text[PAYLOAD_LEN];
};
static InboundMsg in_history[16];
static int ih_count = 0;

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

void in_record(uint8_t seq, uint8_t hop, const char *text) {
    int idx = ih_count % 16;
    in_history[idx].seq = seq;
    in_history[idx].hop = hop;
    strncpy(in_history[idx].text, text, PAYLOAD_LEN - 1);
    ih_count++;
}

static inline void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, FHSS_SEED, blacklist);
    if (ch != last_channel) {
        radio.setChannel(ch);
        last_channel = ch;
        stats_current_ch = ch;
    }
}

// ---------------- On-Board HTTP Server ----------------
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>HopperNet Node C</title><style>body{background:#090d16;color:#f9fafb;font-family:sans-serif;padding:20px;text-align:center;}.card{background:#111827;border:1px solid #243044;border-radius:10px;padding:20px;max-width:400px;margin:auto;}input,button{padding:10px;border-radius:6px;margin:5px;border:none;font-size:16px;}input{background:#1f293d;color:#fff;width:80%;}button{background:#10b981;color:#fff;cursor:pointer;font-weight:bold;}.stat{display:flex;justify-content:space-between;margin:8px 0;font-family:monospace;color:#34d399;}</style></head><body>";
    html += "<div class='card'><h2>HOPPERNET NODE C</h2><p style='color:#9ca3af;'>SSID: " NODE_C_SSID "</p><hr style='border-color:#243044;margin:15px 0;'>";
    html += "<div class='stat'><span>SYNC STATUS:</span><span>" + String(synced ? "LOCKED" : "SCANNING") + "</span></div>";
    html += "<div class='stat'><span>CHANNEL:</span><span>CH " + String(stats_current_ch) + "</span></div>";
    html += "<div class='stat'><span>HOP:</span><span>" + String(stats_current_hop) + "</span></div>";
    html += "<div class='stat'><span>RECEIVED / SENT:</span><span>" + String(stats_received) + " / " + String(stats_sent) + "</span></div>";
    html += "<form method='POST' action='/send'><input type='text' name='msg' placeholder='Type reply...' maxlength='23'><br><button type='submit'>TRANSMIT (C &rarr; A)</button></form></div></body></html>";
    server.send(200, "text/html", html);
}

void handleSend() {
    if (server.hasArg("msg")) {
        String msg = server.arg("msg");
        out_push(msg.c_str(), msg.length());
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleApiStatus() {
    String json = "{\"node\":\"node_c\",\"ssid\":\"" NODE_C_SSID "\",\"synced\":" + String(synced ? "true" : "false") + ",\"ch\":" + String(stats_current_ch) + ",\"hop\":" + String(stats_current_hop) + ",\"sent\":" + String(stats_sent) + ",\"received\":" + String(stats_received) + ",\"acked\":" + String(stats_acked) + "}";
    server.send(200, "application/json", json);
}

void handleApiSend() {
    if (server.hasArg("plain")) {
        String body = server.arg("plain");
        out_push(body.c_str(), body.length());
        server.send(200, "application/json", "{\"status\":\"queued\"}");
    } else {
        server.send(400, "text/plain", "Missing message body");
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(1000);
    Serial.println(F("=========================================="));
    Serial.println(F("  HopperNet NODE C — Dest (SSID: hopperc) "));
    Serial.println(F("=========================================="));

    // Start SoftAP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(NODE_C_SSID, WIFI_PASS_COMMON);
    Serial.print(F("[WIFI] Access Point Started: "));
    Serial.println(NODE_C_SSID);
    Serial.print(F("[WIFI] Web Portal IP: http://"));
    Serial.println(WiFi.softAPIP());

    server.on("/", handleRoot);
    server.on("/send", HTTP_POST, handleSend);
    server.on("/api/status", handleApiStatus);
    server.on("/api/send", HTTP_POST, handleApiSend);
    server.begin();

    blacklist_clear_all(blacklist);
    blacklist_clear_all(rx_blacklist);

    if (!radio.begin()) {
        Serial.println(F("[NODE_C] RF24 init FAILED — check wiring!"));
        while (1) delay(100);
    }

    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(MAX_FRAME_LEN);
    radio.setAutoAck(false);
    radio.startListening();

    Serial.println(F("[NODE_C] Scanning channels for SYNC beacon..."));
}

// ---------------- Loop ----------------
void loop() {
    server.handleClient();

    // 1. Read Commands from USB Serial (Local Desktop App)
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.startsWith("SEND:")) {
            String msg = input.substring(5);
            if (out_push(msg.c_str(), msg.length())) {
                Serial.print(F("[NODE_C] QUEUED RETURN TX: \""));
                Serial.print(msg);
                Serial.println(F("\""));
            }
        }
    }

    // 2. Process Received Radio Frames (SYNC, ACKs, Forward Data from Node B)
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
                    Serial.println(F("[NODE_C] *** SYNC ACQUIRED ***"));
                }
            } else if (f.type == FRAME_TYPE_ACK && f.src == RELAY && f.dst == ROLE) {
                stats_acked++;
            } else if (f.type == FRAME_TYPE_DATA && f.dst == ROLE) {
                // Forward packet delivered from Node A via Relay
                stats_received++;
                uint8_t len = f.payload[0];
                char msg[25] = {0};
                if (len > PAYLOAD_LEN - 1) len = PAYLOAD_LEN - 1;
                memcpy(msg, &f.payload[1], len);

                in_record(f.seq, f.hop_index, msg);
                Serial.print(F("[NODE_C] RECV FORWARD hop="));
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

    // 3. Transmit Window: Reverse Slot [PHASE_FWD_US, 18ms)
    static uint32_t last_tx_hop_c = 0xFFFFFFFF;
    if (phase >= PHASE_FWD_US && phase < 18000 && hop != last_tx_hop_c) {
        last_tx_hop_c = hop;

        OutboundMsg outMsg;
        if (out_pop(&outMsg)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = RELAY;
            tx.seq = seq_counter_c++;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = outMsg.len;
            memcpy(&tx.payload[1], outMsg.text, outMsg.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            stats_sent++;
            radio.startListening();

            Serial.print(F("[NODE_C] TX REVERSE seq="));
            Serial.print(tx.seq);
            Serial.print(F(" data=\""));
            Serial.print(outMsg.text);
            Serial.println(F("\""));
        }
    }
}
