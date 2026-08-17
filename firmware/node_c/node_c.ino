// HopperNet Node C — Destination Endpoint (ESP32)
// 100% Local & Cloudless: Slotted FHSS Mesh + Live Auto-Refreshing Web Portal

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
#define SRC_NODE        NODE_A
#define BAUD            115200

#define QUEUE_SIZE      32
#define HISTORY_SIZE    10

RF24 radio(RF_CE_PIN, RF_CSN_PIN);
WebServer server(80);

// ---------------- State Variables ----------------
static uint8_t  blacklist[BLACKLIST_SIZE];
static uint8_t  rx_blacklist[BLACKLIST_SIZE];
static uint8_t  last_channel = 255;
static uint8_t  scan_ch = CHANNEL_BASE;
static uint32_t rx_hop_index = 0;
static uint32_t rx_master_ts = 0;
static int32_t  clock_offset = 0;
static uint8_t  synced = 0;
static uint32_t last_sync_time_ms = 0;
static uint8_t  seq_counter_c = 1;

// Telemetry Stats
static uint32_t stats_sent = 0;
static uint32_t stats_acked = 0;
static uint32_t stats_received = 0;
static uint32_t stats_current_hop = 0;
static uint8_t  stats_current_ch = 0;

// Message Buffers
struct OutboundMsg {
    char text[PAYLOAD_LEN];
    uint8_t len;
};

struct InboundMsg {
    char text[PAYLOAD_LEN];
    uint8_t seq;
    uint32_t hop;
    unsigned long timestamp_ms;
};

static OutboundMsg out_queue_c[QUEUE_SIZE];
static volatile int oqc_head = 0;
static volatile int oqc_tail = 0;
static volatile int oqc_count = 0;

static InboundMsg in_history[HISTORY_SIZE];
static volatile int ih_count = 0;

bool out_push(const char *text, uint8_t len) {
    if (oqc_count < QUEUE_SIZE) {
        memset(out_queue_c[oqc_head].text, 0, PAYLOAD_LEN);
        out_queue_c[oqc_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        memcpy(out_queue_c[oqc_head].text, text, out_queue_c[oqc_head].len);
        oqc_head = (oqc_head + 1) % QUEUE_SIZE;
        oqc_count++;
        return true;
    }
    return false;
}

bool out_peek(OutboundMsg *out) {
    if (oqc_count > 0) {
        *out = out_queue_c[oqc_tail];
        return true;
    }
    return false;
}

void out_pop() {
    if (oqc_count > 0) {
        oqc_tail = (oqc_tail + 1) % QUEUE_SIZE;
        oqc_count--;
    }
}

void in_record(uint8_t seq, uint32_t hop, const char *text) {
    int idx = ih_count % HISTORY_SIZE;
    in_history[idx].seq = seq;
    in_history[idx].hop = hop;
    in_history[idx].timestamp_ms = millis();
    memset(in_history[idx].text, 0, PAYLOAD_LEN);
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

// ---------------- Embedded Web Portal (Live Auto-Refresh) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>HopperNet — Node C (Destination)</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0b0f19; color: #e2e8f0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; padding: 16px; }
  .container { max-width: 480px; margin: 0 auto; }
  .card { background: #131b2e; border: 1px solid #23304d; border-radius: 12px; padding: 18px; margin-bottom: 14px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
  .header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #23304d; padding-bottom: 10px; margin-bottom: 12px; }
  .title { font-size: 18px; font-weight: 700; color: #34d399; letter-spacing: 0.5px; }
  .badge { font-size: 12px; font-weight: 600; padding: 4px 8px; border-radius: 20px; text-transform: uppercase; }
  .badge-locked { background: #065f46; color: #34d399; }
  .badge-scan { background: #854d0e; color: #facc15; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 12px; }
  .stat-box { background: #1a243b; padding: 10px; border-radius: 8px; border: 1px solid #2a3a5e; }
  .stat-label { font-size: 11px; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
  .stat-val { font-size: 17px; font-weight: 700; color: #f8fafc; font-family: monospace; margin-top: 2px; }
  .input-row { display: flex; gap: 8px; margin-top: 8px; }
  input[type="text"] { flex: 1; background: #1a243b; border: 1px solid #2a3a5e; color: #f8fafc; padding: 12px; border-radius: 8px; font-size: 15px; outline: none; }
  input[type="text"]:focus { border-color: #34d399; }
  button { background: #059669; color: #fff; border: none; padding: 12px 18px; border-radius: 8px; font-size: 15px; font-weight: 600; cursor: pointer; transition: background 0.2s; }
  button:active { background: #047857; }
  .msg-list { list-style: none; max-height: 180px; overflow-y: auto; }
  .msg-item { background: #1a243b; border-left: 3px solid #34d399; padding: 8px 10px; margin-bottom: 6px; border-radius: 4px; font-size: 13px; display: flex; justify-content: space-between; }
  .empty { color: #64748b; font-size: 13px; text-align: center; padding: 12px 0; }
  .live-dot { width: 8px; height: 8px; background: #34d399; border-radius: 50%; display: inline-block; margin-right: 6px; animation: pulse 1.5s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div>
        <div class="title">HOPPERNET NODE C</div>
        <div style="font-size:12px;color:#94a3b8;margin-top:2px;">SSID: hopperc &bull; 192.168.4.1</div>
      </div>
      <div id="sync-badge" class="badge badge-scan">SCANNING</div>
    </div>
    
    <div class="grid">
      <div class="stat-box">
        <div class="stat-label">CHANNEL / FREQ</div>
        <div class="stat-val" id="val-ch">CH --</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">MASTER HOP</div>
        <div class="stat-val" id="val-hop">#0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">FORWARD RECV</div>
        <div class="stat-val" id="val-recv" style="color:#34d399;">0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">RETURN SENT</div>
        <div class="stat-val" id="val-sent" style="color:#38bdf8;">0</div>
      </div>
    </div>

    <div style="font-size:12px;font-weight:600;color:#94a3b8;margin-bottom:6px;text-transform:uppercase;">Reply / Return Alert (C &rarr; B &rarr; A)</div>
    <form id="send-form" onsubmit="sendMsg(event)">
      <div class="input-row">
        <input type="text" id="msg-input" placeholder="Type return message..." maxlength="23" autocomplete="off" required>
        <button type="submit" id="send-btn">TRANSMIT</button>
      </div>
    </form>
    <div id="send-status" style="font-size:12px;color:#34d399;margin-top:6px;min-height:16px;"></div>
  </div>

  <div class="card">
    <div class="header" style="margin-bottom:8px;">
      <div style="font-size:14px;font-weight:600;color:#cbd5e1;"><span class="live-dot"></span>RECEIVED MESSAGES (From Node A)</div>
      <div id="queue-status" style="font-size:12px;color:#94a3b8;">Return Queue: 0</div>
    </div>
    <div id="msg-feed" class="msg-list">
      <div class="empty">No messages received yet.</div>
    </div>
  </div>
</div>

<script>
let lastHistoryCount = -1;

async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    const d = await res.json();
    
    const badge = document.getElementById('sync-badge');
    if (d.synced) {
      badge.className = 'badge badge-locked';
      badge.textContent = 'LOCKED';
    } else {
      badge.className = 'badge badge-scan';
      badge.textContent = 'SCANNING';
    }
    
    document.getElementById('val-ch').textContent = 'CH ' + d.ch + ' (' + (2400 + d.ch) + 'M)';
    document.getElementById('val-hop').textContent = '#' + d.hop;
    document.getElementById('val-recv').textContent = d.received;
    document.getElementById('val-sent').textContent = d.sent + ' (' + d.acked + ' ack)';
    document.getElementById('queue-status').textContent = 'Return Queue: ' + d.out_queue;

    if (d.history_count !== lastHistoryCount) {
      lastHistoryCount = d.history_count;
      renderMessages(d.history);
    }
  } catch(e) {}
}

function renderMessages(history) {
  const feed = document.getElementById('msg-feed');
  if (!history || history.length === 0) {
    feed.innerHTML = '<div class="empty">No messages received yet.</div>';
    return;
  }
  let html = '';
  for (let i = history.length - 1; i >= 0; i--) {
    const m = history[i];
    html += '<div class="msg-item"><div><strong>' + escapeHtml(m.text) + '</strong></div><div style="font-family:monospace;font-size:11px;color:#94a3b8;">#' + m.seq + ' (Hop ' + m.hop + ')</div></div>';
  }
  feed.innerHTML = html;
}

function escapeHtml(s) {
  return s.replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
}

async function sendMsg(e) {
  e.preventDefault();
  const input = document.getElementById('msg-input');
  const text = input.value.trim();
  if (!text) return;
  
  const statusDiv = document.getElementById('send-status');
  statusDiv.textContent = 'Queueing return message...';
  
  try {
    const res = await fetch('/api/send', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'msg=' + encodeURIComponent(text)
    });
    if (res.ok) {
      input.value = '';
      statusDiv.textContent = '✓ Queued! Transmitting to Node B on reverse slot.';
      setTimeout(() => { statusDiv.textContent = ''; }, 3000);
      fetchStatus();
    }
  } catch(err) {
    statusDiv.textContent = 'Error sending message.';
  }
}

setInterval(fetchStatus, 400);
fetchStatus();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiStatus() {
    String json = "{";
    json += "\"node\":\"node_c\",";
    json += "\"synced\":" + String(synced ? "true" : "false") + ",";
    json += "\"ch\":" + String(stats_current_ch) + ",";
    json += "\"hop\":" + String(stats_current_hop) + ",";
    json += "\"sent\":" + String(stats_sent) + ",";
    json += "\"acked\":" + String(stats_acked) + ",";
    json += "\"received\":" + String(stats_received) + ",";
    json += "\"out_queue\":" + String(oqc_count) + ",";
    json += "\"history_count\":" + String(ih_count) + ",";
    json += "\"history\":[";
    
    int count = (ih_count < HISTORY_SIZE) ? ih_count : HISTORY_SIZE;
    int start = (ih_count < HISTORY_SIZE) ? 0 : (ih_count % HISTORY_SIZE);
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        if (i > 0) json += ",";
        json += "{\"seq\":" + String(in_history[idx].seq) + ",";
        json += "\"hop\":" + String(in_history[idx].hop) + ",";
        json += "\"text\":\"" + String(in_history[idx].text) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleApiSend() {
    String msg = "";
    if (server.hasArg("msg")) {
        msg = server.arg("msg");
    } else if (server.hasArg("plain")) {
        msg = server.arg("plain");
    }

    if (msg.length() > 0) {
        out_push(msg.c_str(), msg.length());
        Serial.print(F("[NODE_C] WEB QUEUED RETURN: \""));
        Serial.print(msg);
        Serial.println(F("\""));
        server.send(200, "application/json", "{\"status\":\"queued\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"empty message\"}");
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(500);
    Serial.println(F("=========================================="));
    Serial.println(F("  HopperNet NODE C — Dest (SSID: hopperc)"));
    Serial.println(F("=========================================="));

    // 1. SoftAP Setup (Isolated Channel 11, 75% Power)
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_15dBm);
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(NODE_C_SSID, WIFI_PASS_COMMON, 11, 0, 4);

    Serial.print(F("[WIFI] Access Point: "));
    Serial.println(NODE_C_SSID);
    Serial.println(F("[WIFI] Web Portal: http://192.168.4.1"));

    server.on("/", handleRoot);
    server.on("/api/status", handleApiStatus);
    server.on("/api/send", HTTP_POST, handleApiSend);
    server.begin();

    blacklist_clear_all(blacklist);
    blacklist_clear_all(rx_blacklist);

    // 2. Initialize Radio
    if (!radio.begin()) {
        Serial.println(F("[NODE_C] WARNING: RF24 init FAILED — check wiring!"));
    } else {
        radio.setPALevel(RF24_PA_HIGH);
        radio.setDataRate(RF24_250KBPS);
        radio.setPayloadSize(MAX_FRAME_LEN);
        radio.setAutoAck(false);
        radio.setCRCLength(RF24_CRC_16);
        radio.openWritingPipe(FHSS_PIPE_ADDR);
        radio.openReadingPipe(1, FHSS_PIPE_ADDR);
        radio.startListening();
        Serial.println(F("[NODE_C] RF24 Initialized with pipe HOPP1. Scanning channels for SYNC..."));
    }
}

// ---------------- Loop ----------------
void loop() {
    server.handleClient();

    // 1. Read Serial Commands from Desktop App
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

    // 2. Check for Incoming Radio Frames (SYNC, ACKs, Forward Data from Node B)
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
                last_sync_time_ms = millis();

                if (!synced) {
                    synced = 1;
                    set_current_channel(rx_hop_index + 1);
                    Serial.print(F("[NODE_C] *** SYNC ACQUIRED *** Master Hop: "));
                    Serial.println(rx_hop_index);
                }
            } else if (f.type == FRAME_TYPE_ACK && f.src == RELAY && f.dst == ROLE) {
                stats_acked++;
                out_pop(); // Return message was safely received by Node B!
                Serial.print(F("[NODE_C] ACK RECEIVED from Relay for seq="));
                Serial.println(f.seq);
            } else if (f.type == FRAME_TYPE_DATA && f.dst == ROLE) {
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

                // Immediate ACK back to Relay
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = RELAY;
                ack.seq = f.seq;
                ack.hop_index = f.hop_index;
                ack.payload[0] = f.seq;
                frame_fill_crc(&ack, PAYLOAD_LEN);

                radio.stopListening();
                radio.write(&ack, MAX_FRAME_LEN);
                radio.startListening();
            }
        }
    }

    // Sync Timeout Watchdog
    if (synced && (millis() - last_sync_time_ms > 2500)) {
        synced = 0;
        Serial.println(F("[NODE_C] SYNC LOST — Returning to channel scan..."));
    }

    // If Unsynced: Park on each channel for 80ms to catch the master beacon
    static uint32_t last_scan_switch_ms = 0;
    if (!synced) {
        if (millis() - last_scan_switch_ms >= 80) {
            last_scan_switch_ms = millis();
            radio.setChannel(scan_ch);
            scan_ch = (scan_ch + 1) % (CHANNEL_BASE + NUM_CHANNELS);
        }
        return;
    }

    // 3. Synced Execution: Hop in exact lockstep
    uint32_t now_master = (uint32_t)((int32_t)micros() + clock_offset);
    uint32_t hop = now_master / DWELL_US;
    uint32_t phase = now_master % DWELL_US;
    stats_current_hop = hop;

    set_current_channel(hop);

    // 4. Reverse Transmit Window: [13.5ms, 18.5ms)
    static uint32_t last_tx_hop_c = 0xFFFFFFFF;
    if (phase >= 13500 && phase < 18500 && hop != last_tx_hop_c) {
        last_tx_hop_c = hop;

        OutboundMsg outMsg;
        if (out_peek(&outMsg)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = RELAY;
            tx.seq = seq_counter_c;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = outMsg.len;
            memcpy(&tx.payload[1], outMsg.text, outMsg.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            stats_sent++;
            radio.startListening();

            Serial.print(F("[NODE_C] TX RETURN hop="));
            Serial.print(hop);
            Serial.print(F(" seq="));
            Serial.print(seq_counter_c);
            Serial.print(F(" text=\""));
            Serial.print(outMsg.text);
            Serial.println(F("\""));
        }
    }
}
