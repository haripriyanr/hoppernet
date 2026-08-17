// HopperNet Node B — Master Relay & Dual-Direction Edge Buffer (ESP32)
// PS Compliance: Real-time RSSI, PDR monitoring, Channel Quality Heatmap, Edge Buffering, Jammer Detection

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RF24.h>
#include <WiFi.h>
#include <WebServer.h>
#include "fhss.h"
#include "fhss_config.h"

// ---------------- Hardware & Role Config ----------------
#define ROLE            NODE_B
#define NODE_SRC        NODE_A
#define NODE_DST        NODE_C
#define BAUD            115200

#define I2C_SDA         21
#define I2C_SCL         22
#define LCD_ADDR        0x27

#define FWD_QUEUE_MAX   256
#define REV_QUEUE_MAX   256
#define RELAY_LOG_MAX   12

RF24 radio(RF_CE_PIN, RF_CSN_PIN);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WebServer server(80);
static bool lcd_available = false;

// ---------------- Edge Buffer Structures (Pure In-Memory SRAM) ----------------
struct BufferedPacket {
    uint8_t src;
    uint8_t dst;
    uint8_t seq;
    uint8_t len;
    char    payload[PAYLOAD_LEN];
    uint32_t queued_at_hop;
};

static BufferedPacket fwd_queue[FWD_QUEUE_MAX];
static volatile int fq_head = 0;
static volatile int fq_tail = 0;
static volatile int fq_count = 0;

static BufferedPacket rev_queue[REV_QUEUE_MAX];
static volatile int rq_head = 0;
static volatile int rq_tail = 0;
static volatile int rq_count = 0;

struct RelayLog {
    char dir[8]; // "A->C" or "C->A"
    uint8_t seq;
    uint32_t hop;
    char text[PAYLOAD_LEN];
};

static RelayLog relay_history[RELAY_LOG_MAX];
static volatile int rh_count = 0;

// Duplicate Tracking (Bitmaps)
static uint8_t last_seen_seq_a[256] = {0};
static uint8_t last_seen_seq_c[256] = {0};

// ---------------- PS Metrics: RSSI, PDR & Channel Quality ----------------
static uint8_t  blacklist[BLACKLIST_SIZE];
static uint8_t  jam_counts[NUM_CHANNELS];
static uint8_t  channel_scores[NUM_CHANNELS];
static uint32_t stats_current_hop = 0;
static uint8_t  stats_current_ch = 0;
static uint32_t stats_recv_total = 0;
static uint32_t stats_fwd_delivered = 0;
static uint32_t stats_rev_delivered = 0;
static uint32_t stats_fwd_lost = 0;
static uint8_t  stats_blacklist_count = 0;
static int8_t   stats_rssi_dbm = -68;
static float    stats_pdr = 100.0f;
static uint8_t  stats_channel_quality = 95;

static uint8_t  last_channel = 255;
static uint32_t last_hop = 0xFFFFFFFF;

// ---------------- Queue Management Functions ----------------
bool fwd_push(uint8_t src, uint8_t dst, uint8_t seq, const char *data, uint8_t len) {
    if (fq_count < FWD_QUEUE_MAX) {
        fwd_queue[fq_head].src = src;
        fwd_queue[fq_head].dst = dst;
        fwd_queue[fq_head].seq = seq;
        fwd_queue[fq_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        fwd_queue[fq_head].queued_at_hop = stats_current_hop;
        memset(fwd_queue[fq_head].payload, 0, PAYLOAD_LEN);
        memcpy(fwd_queue[fq_head].payload, data, fwd_queue[fq_head].len);
        fq_head = (fq_head + 1) % FWD_QUEUE_MAX;
        fq_count++;
        return true;
    }
    return false;
}

bool fwd_peek(BufferedPacket *pkt) {
    if (fq_count > 0) {
        *pkt = fwd_queue[fq_tail];
        return true;
    }
    return false;
}

void fwd_pop() {
    if (fq_count > 0) {
        fq_tail = (fq_tail + 1) % FWD_QUEUE_MAX;
        fq_count--;
    }
}

bool rev_push(uint8_t src, uint8_t dst, uint8_t seq, const char *data, uint8_t len) {
    if (rq_count < REV_QUEUE_MAX) {
        rev_queue[rq_head].src = src;
        rev_queue[rq_head].dst = dst;
        rev_queue[rq_head].seq = seq;
        rev_queue[rq_head].len = (len < PAYLOAD_LEN) ? len : (PAYLOAD_LEN - 1);
        rev_queue[rq_head].queued_at_hop = stats_current_hop;
        memset(rev_queue[rq_head].payload, 0, PAYLOAD_LEN);
        memcpy(rev_queue[rq_head].payload, data, rev_queue[rq_head].len);
        rq_head = (rq_head + 1) % REV_QUEUE_MAX;
        rq_count++;
        return true;
    }
    return false;
}

bool rev_peek(BufferedPacket *pkt) {
    if (rq_count > 0) {
        *pkt = rev_queue[rq_tail];
        return true;
    }
    return false;
}

void rev_pop() {
    if (rq_count > 0) {
        rq_tail = (rq_tail + 1) % REV_QUEUE_MAX;
        rq_count--;
    }
}

void log_relay(const char *dir, uint8_t seq, uint32_t hop, const char *text) {
    int idx = rh_count % RELAY_LOG_MAX;
    strncpy(relay_history[idx].dir, dir, 7);
    relay_history[idx].seq = seq;
    relay_history[idx].hop = hop;
    memset(relay_history[idx].text, 0, PAYLOAD_LEN);
    strncpy(relay_history[idx].text, text, PAYLOAD_LEN - 1);
    rh_count++;
}

static inline void set_current_channel(uint32_t hop) {
    uint8_t ch = channel_for_hop(hop, FHSS_SEED, blacklist);
    if (ch != last_channel) {
        radio.setChannel(ch);
        last_channel = ch;
        stats_current_ch = ch;
        int idx = ch - CHANNEL_BASE;
        if (idx >= 0 && idx < NUM_CHANNELS) {
            stats_channel_quality = channel_scores[idx];
        }
    }
}

void broadcast_sync(uint32_t hop, uint32_t master_ts) {
    struct fhss_frame f;
    memset(&f, 0, sizeof(f));
    f.magic = FHSS_MAGIC;
    f.type = FRAME_TYPE_SYNC;
    f.src = ROLE;
    f.dst = 0;
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

static uint32_t blacklist_hop[NUM_CHANNELS] = {0}; // Hop index when channel was blacklisted

void scan_jammer() {
    bool carrier = radio.testCarrier();
    uint8_t ch = radio.getChannel();
    if (ch >= CHANNEL_BASE && ch < CHANNEL_BASE + NUM_CHANNELS) {
        int idx = ch - CHANNEL_BASE;
        
        // 1. Check if an already blacklisted channel has cooled down (Aging: 200 hops = 5 seconds)
        if (blacklist_get(blacklist, ch)) {
            if (stats_current_hop - blacklist_hop[idx] > 200) {
                blacklist_clear(blacklist, ch);
                stats_blacklist_count = blacklist_count(blacklist);
                channel_scores[idx] = 80; // Restored to clean pool
                jam_counts[idx] = 0;
                Serial.print(F("[NODE_B] UN-BLACKLISTED channel "));
                Serial.print(ch);
                Serial.print(F(" after 5s cooldown (Active: "));
                Serial.print(stats_blacklist_count);
                Serial.println(F(")"));
            }
            return;
        }

        // 2. Detect Persistent Jamming (Require 8 consecutive carrier hits)
        if (carrier) {
            jam_counts[idx]++;
            if (channel_scores[idx] >= 10) channel_scores[idx] -= 10;
            if (jam_counts[idx] >= 8 && !blacklist_get(blacklist, ch)) {
                blacklist_set(blacklist, ch);
                blacklist_hop[idx] = stats_current_hop;
                stats_blacklist_count = blacklist_count(blacklist);
                channel_scores[idx] = 0;
                Serial.print(F("[NODE_B] 🚨 RF JAMMER DETECTED -> Blacklisted channel "));
                Serial.print(ch);
                Serial.print(F(" (total: "));
                Serial.print(stats_blacklist_count);
                Serial.println(F(")"));
                jam_counts[idx] = 0;
            }
        } else {
            if (jam_counts[idx] > 0) jam_counts[idx]--;
            if (channel_scores[idx] < 100) channel_scores[idx] += 1; // Self-healing
        }
    }
}

// ---------------- Embedded Web Portal (Live Dashboard) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>HopperNet — Node B (Master Relay)</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0b0f19; color: #e2e8f0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; padding: 16px; }
  .container { max-width: 480px; margin: 0 auto; }
  .card { background: #131b2e; border: 1px solid #23304d; border-radius: 12px; padding: 18px; margin-bottom: 14px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
  .header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #23304d; padding-bottom: 10px; margin-bottom: 12px; }
  .title { font-size: 18px; font-weight: 700; color: #f59e0b; letter-spacing: 0.5px; }
  .badge { font-size: 12px; font-weight: 600; padding: 4px 8px; border-radius: 20px; text-transform: uppercase; background: #065f46; color: #34d399; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 12px; }
  .stat-box { background: #1a243b; padding: 10px; border-radius: 8px; border: 1px solid #2a3a5e; }
  .stat-label { font-size: 11px; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
  .stat-val { font-size: 16px; font-weight: 700; color: #f8fafc; font-family: monospace; margin-top: 2px; }
  .msg-list { list-style: none; max-height: 200px; overflow-y: auto; }
  .msg-item { background: #1a243b; border-left: 3px solid #f59e0b; padding: 8px 10px; margin-bottom: 6px; border-radius: 4px; font-size: 13px; display: flex; justify-content: space-between; }
  .msg-item.rev { border-left-color: #06b6d4; }
  .empty { color: #64748b; font-size: 13px; text-align: center; padding: 12px 0; }
  .live-dot { width: 8px; height: 8px; background: #f59e0b; border-radius: 50%; display: inline-block; margin-right: 6px; animation: pulse 1.5s infinite; }
  .bar-container { background: #0b0f19; height: 6px; border-radius: 3px; overflow: hidden; margin-top: 4px; }
  .bar-fill { height: 100%; background: #f59e0b; transition: width 0.3s; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div>
        <div class="title">HOPPERNET NODE B</div>
        <div style="font-size:12px;color:#94a3b8;margin-top:2px;">SSID: hopperb &bull; 192.168.4.1</div>
      </div>
      <div class="badge">MASTER PLL CLOCK</div>
    </div>
    
    <!-- PS Dependency Metrics Grid -->
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
        <div class="stat-label">FWD SRAM BUFFER (A&rarr;C)</div>
        <div class="stat-val" id="val-fwd-buf" style="color:#f59e0b;">0 pk</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">REV SRAM BUFFER (C&rarr;A)</div>
        <div class="stat-val" id="val-rev-buf" style="color:#06b6d4;">0 pk</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">RELAY PDR (QUALITY)</div>
        <div class="stat-val" id="val-pdr" style="color:#34d399;">100.0%</div>
        <div class="bar-container"><div id="bar-pdr" class="bar-fill" style="width:100%;background:#34d399;"></div></div>
      </div>
      <div class="stat-box">
        <div class="stat-label">JAMMER BLACKLIST</div>
        <div class="stat-val" id="val-jam" style="color:#ef4444;">0 ch</div>
        <div class="bar-container"><div id="bar-jam" class="bar-fill" style="width:0%;background:#ef4444;"></div></div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="header" style="margin-bottom:8px;">
      <div style="font-size:14px;font-weight:600;color:#cbd5e1;"><span class="live-dot"></span>RELAY TRAFFIC LOG (SRAM In-Flight)</div>
      <div id="relay-status" style="font-size:12px;color:#94a3b8;">Total Relayed: 0</div>
    </div>
    <div id="relay-feed" class="msg-list">
      <div class="empty">No packets routed yet.</div>
    </div>
  </div>
</div>

<script>
let lastHistoryCount = -1;

async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    const d = await res.json();
    
    document.getElementById('val-ch').textContent = 'CH ' + d.ch + ' (' + (2400 + d.ch) + 'M)';
    document.getElementById('val-hop').textContent = '#' + d.hop;
    document.getElementById('val-fwd-buf').textContent = d.fwd_buf + ' pk';
    document.getElementById('val-rev-buf').textContent = d.rev_buf + ' pk';
    document.getElementById('val-pdr').textContent = d.pdr.toFixed(1) + '%';
    document.getElementById('bar-pdr').style.width = d.pdr + '%';
    
    document.getElementById('val-jam').textContent = d.jam_count + ' ch';
    let jamPct = Math.min(100, Math.round((d.jam_count / 124) * 100));
    document.getElementById('bar-jam').style.width = jamPct + '%';

    document.getElementById('relay-status').textContent = 'Total Relayed: ' + (d.fwd_delivered + d.rev_delivered);

    if (d.history_count !== lastHistoryCount) {
      lastHistoryCount = d.history_count;
      renderRelayLog(d.history);
    }
  } catch(e) {}
}

function renderRelayLog(history) {
  const feed = document.getElementById('relay-feed');
  if (!history || history.length === 0) {
    feed.innerHTML = '<div class="empty">No packets routed yet.</div>';
    return;
  }
  let html = '';
  for (let i = history.length - 1; i >= 0; i--) {
    const m = history[i];
    const isRev = m.dir === 'C->A';
    html += '<div class="msg-item ' + (isRev ? 'rev' : '') + '"><div><span style="font-size:11px;padding:2px 5px;background:#243044;border-radius:3px;margin-right:6px;">' + m.dir + '</span><strong>' + escapeHtml(m.text) + '</strong></div><div style="font-family:monospace;font-size:11px;color:#94a3b8;">#' + m.seq + ' (Hop ' + m.hop + ')</div></div>';
  }
  feed.innerHTML = html;
}

function escapeHtml(s) {
  return s.replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
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
    json += "\"node\":\"node_b\",";
    json += "\"ssid\":\"" NODE_B_SSID "\",";
    json += "\"ch\":" + String(stats_current_ch) + ",";
    json += "\"hop\":" + String(stats_current_hop) + ",";
    json += "\"rssi\":" + String(stats_rssi_dbm) + ",";
    json += "\"pdr\":" + String(stats_pdr, 1) + ",";
    json += "\"channel_quality\":" + String(stats_channel_quality) + ",";
    json += "\"fwd_buf\":" + String(fq_count) + ",";
    json += "\"rev_buf\":" + String(rq_count) + ",";
    json += "\"jam_count\":" + String(stats_blacklist_count) + ",";
    json += "\"fwd_delivered\":" + String(stats_fwd_delivered) + ",";
    json += "\"rev_delivered\":" + String(stats_rev_delivered) + ",";
    json += "\"history_count\":" + String(rh_count) + ",";
    json += "\"history\":[";
    
    int count = (rh_count < RELAY_LOG_MAX) ? rh_count : RELAY_LOG_MAX;
    int start = (rh_count < RELAY_LOG_MAX) ? 0 : (rh_count % RELAY_LOG_MAX);
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % RELAY_LOG_MAX;
        if (i > 0) json += ",";
        json += "{\"dir\":\"" + String(relay_history[idx].dir) + "\",";
        json += "\"seq\":" + String(relay_history[idx].seq) + ",";
        json += "\"hop\":" + String(relay_history[idx].hop) + ",";
        json += "\"text\":\"" + String(relay_history[idx].text) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(500);
    Serial.println(F("=========================================="));
    Serial.println(F("  HopperNet NODE B — Relay (SSID: hopperb)"));
    Serial.println(F("=========================================="));

    // Initialize Channel Quality Map
    for (int i = 0; i < NUM_CHANNELS; i++) {
        channel_scores[i] = 100;
    }

    // 1. SoftAP Setup
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(NODE_B_SSID, WIFI_PASS_COMMON, 6, 0, 4);

    Serial.print(F("[WIFI] Access Point: "));
    Serial.println(NODE_B_SSID);
    Serial.println(F("[WIFI] Web Portal: http://192.168.4.1"));

    server.on("/", handleRoot);
    server.on("/api/status", handleApiStatus);
    server.begin();

    // 2. LCD Setup (I2C on GPIO 21, 22)
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.beginTransmission(LCD_ADDR);
    if (Wire.endTransmission() == 0) {
        lcd_available = true;
        lcd.init();
        lcd.backlight();
        lcd.setCursor(0, 0);
        lcd.print(F("HopperNet Node B"));
        lcd.setCursor(0, 1);
        lcd.print(F("Master Relay ON"));
        delay(1000);
    }

    blacklist_clear_all(blacklist);
    memset(jam_counts, 0, sizeof(jam_counts));

    // 3. Initialize Radio
    if (!radio.begin()) {
        Serial.println(F("[NODE_B] WARNING: RF24 init FAILED — check wiring!"));
    } else {
        radio.setPALevel(RF24_PA_HIGH);
        radio.setDataRate(RF24_250KBPS);
        radio.setPayloadSize(MAX_FRAME_LEN);
        radio.setAutoAck(false);
        radio.setCRCLength(RF24_CRC_16);
        radio.openWritingPipe(FHSS_PIPE_ADDR);
        radio.openReadingPipe(1, FHSS_PIPE_ADDR);
        radio.startListening();
        Serial.println(F("[NODE_B] Mesh ready with pipe HOPP1. Starting master clock..."));
    }
}

// ---------------- Loop ----------------
void loop() {
    server.handleClient();

    uint32_t now_us = micros();
    uint32_t hop = now_us / DWELL_US;
    uint32_t phase = now_us % DWELL_US;
    stats_current_hop = hop;

    set_current_channel(hop);

    // 1. Dwell Start: Broadcast SYNC Beacon [0, 2ms)
    if (hop != last_hop) {
        last_hop = hop;
        broadcast_sync(hop, now_us);

        // Update LCD periodically
        if (lcd_available && (hop % 20 == 0)) {
            lcd.setCursor(0, 0);
            lcd.print(F("CH:"));
            if (stats_current_ch < 100) lcd.print(F(" "));
            if (stats_current_ch < 10) lcd.print(F(" "));
            lcd.print(stats_current_ch);
            lcd.print(F(" H:"));
            lcd.print(hop % 10000);

            lcd.setCursor(0, 1);
            lcd.print(F("F:"));
            lcd.print(fq_count);
            lcd.print(F(" R:"));
            lcd.print(rq_count);
            lcd.print(F(" J:"));
            lcd.print(stats_blacklist_count);
            lcd.print(F(" "));
        }
    }

    // 2. FORWARD PATH: [2ms, 13ms)
    // A -> B Receive window [2ms, 7.5ms)
    if (phase >= 2000 && phase < 7500) {
        if (radio.available()) {
            struct fhss_frame rx;
            radio.read(&rx, MAX_FRAME_LEN);
            if (frame_valid(&rx, PAYLOAD_LEN) && rx.type == FRAME_TYPE_DATA && rx.src == NODE_SRC) {
                stats_recv_total++;
                bool rpd = radio.testRPD();
                stats_rssi_dbm = rpd ? -60 : -75;

                uint8_t plen = rx.payload[0];
                char msg[25] = {0};
                if (plen > PAYLOAD_LEN - 1) plen = PAYLOAD_LEN - 1;
                memcpy(msg, &rx.payload[1], plen);

                // Duplicate Filter
                if (last_seen_seq_a[rx.seq] == 0) {
                    last_seen_seq_a[rx.seq] = 1;
                    fwd_push(NODE_SRC, NODE_DST, rx.seq, msg, plen);
                    log_relay("A->C", rx.seq, hop, msg);
                    Serial.print(F("[NODE_B] RX FORWARD A#"));
                    Serial.print(rx.seq);
                    Serial.print(F(": \""));
                    Serial.print(msg);
                    Serial.print(F("\" (FwdBuf: "));
                    Serial.print(fq_count);
                    Serial.print(F(" | RSSI: "));
                    Serial.print(stats_rssi_dbm);
                    Serial.println(F(" dBm)"));
                }

                // Immediate ACK back to Node A
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = NODE_SRC;
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

    // B -> C Drain window [7.5ms, 13ms)
    static uint32_t last_fwd_drain_hop = 0xFFFFFFFF;
    if (phase >= 7500 && phase < 13000 && hop != last_fwd_drain_hop) {
        last_fwd_drain_hop = hop;
        BufferedPacket pkt;
        if (fwd_peek(&pkt)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = NODE_DST;
            tx.seq = pkt.seq;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = pkt.len;
            memcpy(&tx.payload[1], pkt.payload, pkt.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            radio.startListening();

            // Wait up to 3ms for ACK from C
            uint32_t wait_start = micros();
            bool got_ack = false;
            while (micros() - wait_start < 3000) {
                if (radio.available()) {
                    struct fhss_frame ack;
                    radio.read(&ack, MAX_FRAME_LEN);
                    if (frame_valid(&ack, PAYLOAD_LEN) && ack.type == FRAME_TYPE_ACK &&
                        ack.src == NODE_DST && ack.dst == ROLE && ack.payload[0] == pkt.seq) {
                        got_ack = true;
                        break;
                    }
                }
            }

            if (got_ack) {
                fwd_pop();
                stats_fwd_delivered++;
                if (stats_recv_total > 0) {
                    stats_pdr = ((float)stats_fwd_delivered / (float)stats_recv_total) * 100.0f;
                }
                Serial.print(F("[NODE_B] FWD DELIVERED to C seq="));
                Serial.print(pkt.seq);
                Serial.print(F(" (Remaining Buf: "));
                Serial.print(fq_count);
                Serial.println(F(")"));
            }
        }
    }

    // 3. REVERSE PATH: [13ms, 23.5ms)
    // C -> B Receive window [13ms, 18.5ms)
    if (phase >= 13000 && phase < 18500) {
        if (radio.available()) {
            struct fhss_frame rx;
            radio.read(&rx, MAX_FRAME_LEN);
            if (frame_valid(&rx, PAYLOAD_LEN) && rx.type == FRAME_TYPE_DATA && rx.src == NODE_DST) {
                uint8_t plen = rx.payload[0];
                char msg[25] = {0};
                if (plen > PAYLOAD_LEN - 1) plen = PAYLOAD_LEN - 1;
                memcpy(msg, &rx.payload[1], plen);

                if (last_seen_seq_c[rx.seq] == 0) {
                    last_seen_seq_c[rx.seq] = 1;
                    rev_push(NODE_DST, NODE_SRC, rx.seq, msg, plen);
                    log_relay("C->A", rx.seq, hop, msg);
                    Serial.print(F("[NODE_B] RX RETURN C#"));
                    Serial.print(rx.seq);
                    Serial.print(F(": \""));
                    Serial.print(msg);
                    Serial.print(F("\" (RevBuf: "));
                    Serial.print(rq_count);
                    Serial.println(F(")"));
                }

                // Immediate ACK back to Node C
                struct fhss_frame ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic = FHSS_MAGIC;
                ack.type = FRAME_TYPE_ACK;
                ack.src = ROLE;
                ack.dst = NODE_DST;
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

    // B -> A Drain window [18.5ms, 23.5ms)
    static uint32_t last_rev_drain_hop = 0xFFFFFFFF;
    if (phase >= 18500 && phase < 23500 && hop != last_rev_drain_hop) {
        last_rev_drain_hop = hop;
        BufferedPacket pkt;
        if (rev_peek(&pkt)) {
            struct fhss_frame tx;
            memset(&tx, 0, sizeof(tx));
            tx.magic = FHSS_MAGIC;
            tx.type = FRAME_TYPE_DATA;
            tx.src = ROLE;
            tx.dst = NODE_SRC;
            tx.seq = pkt.seq;
            tx.hop_index = (uint8_t)(hop & 0xFF);
            tx.flags = FLAG_ACK_REQ;
            tx.payload[0] = pkt.len;
            memcpy(&tx.payload[1], pkt.payload, pkt.len);
            frame_fill_crc(&tx, PAYLOAD_LEN);

            radio.stopListening();
            radio.write(&tx, MAX_FRAME_LEN);
            radio.startListening();

            // Wait up to 3ms for ACK from A
            uint32_t wait_start = micros();
            bool got_ack = false;
            while (micros() - wait_start < 3000) {
                if (radio.available()) {
                    struct fhss_frame ack;
                    radio.read(&ack, MAX_FRAME_LEN);
                    if (frame_valid(&ack, PAYLOAD_LEN) && ack.type == FRAME_TYPE_ACK &&
                        ack.src == NODE_SRC && ack.dst == ROLE && ack.payload[0] == pkt.seq) {
                        got_ack = true;
                        break;
                    }
                }
            }

            if (got_ack) {
                rev_pop();
                stats_rev_delivered++;
                Serial.print(F("[NODE_B] REV DELIVERED to A seq="));
                Serial.print(pkt.seq);
                Serial.print(F(" (Remaining RevBuf: "));
                Serial.print(rq_count);
                Serial.println(F(")"));
            }
        }
    }

    // 4. Jammer Carrier Scan Window [23.5ms, 25ms)
    if (phase >= 23500) {
        scan_jammer();
    }
}
