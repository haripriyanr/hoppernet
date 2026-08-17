// HopperNet Node B — SpectrumPipe Single-Radio Master Relay (ESP32)
// 100% Local & Cloudless: 50 ms Slotted Superframe + SRAM Custody Queues + Dual-Core Architecture

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

#define QUEUE_MAX       64
#define RELAY_LOG_MAX   12

RF24 radio(RF_CE_PIN, RF_CSN_PIN);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WebServer server(80);
static bool lcd_available = false;

// ---------------- Thread Safety Spinlocks ----------------
static portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- SRAM Custody Queues ----------------
struct QueueItem {
    DataFrame frame;
    uint32_t queuedSF;
    bool used;
};

static QueueItem qAC[QUEUE_MAX];
static QueueItem qCA[QUEUE_MAX];
static volatile uint8_t qACHead = 0, qACTail = 0, qACCount = 0;
static volatile uint8_t qCAHead = 0, qCATail = 0, qCACount = 0;

struct RelayLog {
    char dir[8]; // "A->C" or "C->A"
    uint16_t msgId;
    uint8_t frag;
    uint32_t sf;
};

static RelayLog relay_history[RELAY_LOG_MAX];
static volatile int rh_count = 0;

// Spectrum & Link Health Tracking
static uint8_t  blacklist[BLACKLIST_BYTES];
static uint8_t  badStreak[RF_CHANNEL_COUNT];
static uint8_t  goodStreak[RF_CHANNEL_COUNT];
static volatile uint16_t mapVersion = 1;
static volatile uint32_t sfNow = 0;
static volatile uint8_t  currentChannel = RF_CHANNEL_SYNC;
static volatile uint32_t rxA = 0, rxC = 0, txC = 0, txA = 0;
static volatile uint32_t ackA = 0, ackC = 0;

// 20 Hops / Sec Spectrum Stream
struct HopRecord {
    uint8_t channel;
    uint8_t matched;
};

static HopRecord sec_hops[HOPS_PER_SEC];
static HopRecord display_hops[HOPS_PER_SEC];
static volatile uint8_t display_matched_count = HOPS_PER_SEC;
static volatile uint32_t last_sec_boundary_sf = 0;

// ---------------- Queue Operations ----------------
bool qPush(QueueItem *q, volatile uint8_t &head, volatile uint8_t &count, const DataFrame &f) {
    portENTER_CRITICAL(&queueMux);
    if (count >= QUEUE_MAX) {
        portEXIT_CRITICAL(&queueMux);
        return false;
    }
    q[head].frame = f;
    q[head].queuedSF = sfNow;
    q[head].used = true;
    head = (head + 1) % QUEUE_MAX;
    count++;
    portEXIT_CRITICAL(&queueMux);
    return true;
}

bool qPeek(QueueItem *q, volatile uint8_t tail, volatile uint8_t count, DataFrame &f) {
    portENTER_CRITICAL(&queueMux);
    if (!count) {
        portEXIT_CRITICAL(&queueMux);
        return false;
    }
    f = q[tail].frame;
    portEXIT_CRITICAL(&queueMux);
    return true;
}

void qPop(volatile uint8_t &tail, volatile uint8_t &count) {
    portENTER_CRITICAL(&queueMux);
    if (count) {
        tail = (tail + 1) % QUEUE_MAX;
        count--;
    }
    portEXIT_CRITICAL(&queueMux);
}

void logRelay(const char *dir, uint16_t msgId, uint8_t frag, uint32_t sf) {
    portENTER_CRITICAL(&queueMux);
    int idx = rh_count % RELAY_LOG_MAX;
    strncpy(relay_history[idx].dir, dir, 7);
    relay_history[idx].msgId = msgId;
    relay_history[idx].frag = frag;
    relay_history[idx].sf = sf;
    rh_count++;
    portEXIT_CRITICAL(&queueMux);
}

// Duplicate Filtering
static uint16_t last_seen_msgId_a = 0xFFFF;
static uint8_t  last_seen_frag_a = 0xFF;
static uint16_t last_seen_msgId_c = 0xFFFF;
static uint8_t  last_seen_frag_c = 0xFF;

// ---------------- Link Quality Scoring & Dynamic Blacklist ----------------
void scoreSuccess(uint8_t ch) {
    if (ch < RF_CHANNEL_FIRST || ch > RF_CHANNEL_LAST) return;
    uint8_t i = ch - RF_CHANNEL_FIRST;
    if (goodStreak[i] < 250) goodStreak[i]++;
    if (badStreak[i] > 0) badStreak[i]--;
    if (blGet(blacklist, ch) && goodStreak[i] >= 8) {
        blSet(blacklist, ch, false);
        goodStreak[i] = 0;
        mapVersion++;
        Serial.printf("EVENT|B|CHANNEL_RECOVER|ch=%u|map=%u\n", ch, mapVersion);
    }
}

void scoreJammerEnergy(uint8_t ch) {
    if (ch < RF_CHANNEL_FIRST || ch > RF_CHANNEL_LAST) return;
    uint8_t i = ch - RF_CHANNEL_FIRST;
    if (badStreak[i] < 250) badStreak[i]++;
    goodStreak[i] = 0;
    // Only blacklist if energy is confirmed repeatedly (>= 5 times) and under 16 total blacklisted
    if (badStreak[i] >= 5 && !blGet(blacklist, ch) && blCount(blacklist) < 16) {
        blSet(blacklist, ch, true);
        mapVersion++;
        Serial.printf("EVENT|B|BLACKLIST_JAMMER|ch=%u|map=%u|total=%u\n", ch, mapVersion, blCount(blacklist));
    }
}

static inline void tune(uint8_t ch) {
    if (currentChannel != ch) {
        setRadioChannel(radio, ch);
        currentChannel = ch;
    }
}

void sendSync() {
    SyncFrame s{};
    s.magic = SP_MAGIC;
    s.version = SP_VERSION;
    s.type = FT_SYNC;
    s.src = NODE_B;
    s.dst = 0;
    s.sf = sfNow;
    s.mapVersion = mapVersion;
    memcpy(s.blacklist, blacklist, BLACKLIST_BYTES);
    uint8_t syncCh = getSyncChannel(sfNow);
    tune(syncCh);
    txFrame(radio, &s);
}

void sendAck(uint8_t dst, uint32_t sf, uint16_t msgId, uint8_t frag, uint8_t type) {
    AckFrame a{};
    a.magic = SP_MAGIC;
    a.version = SP_VERSION;
    a.type = type;
    a.src = NODE_B;
    a.dst = dst;
    a.sf = sf;
    a.msgId = msgId;
    a.frag = frag;
    a.code = (type == FT_DELIVERY ? 2 : 1);
    txFrame(radio, &a);
}

void receiveFromA() {
    uint8_t ch = hopChannel(sfNow, FHSS_SEED_AB, blacklist);
    tune(ch);
    uint32_t deadline = micros() + 9000;
    while ((int32_t)(micros() - deadline) < 0) {
        if (!radio.available()) {
            delayMicroseconds(50);
            continue;
        }
        uint8_t raw[32];
        radio.read(raw, 32);
        DataFrame d;
        memcpy(&d, raw, 32);
        if (d.magic != SP_MAGIC || d.version != SP_VERSION || d.type != FT_DATA || d.src != NODE_A || d.dst != NODE_B || d.len > DATA_PLAINTEXT_MAX) continue;
        
        rxA++;
        // Check for duplicate fragment
        if (d.msgId == last_seen_msgId_a && d.frag == last_seen_frag_a) {
            // Already in custody — re-send custody ACK without duplicating in queue
            sendAck(NODE_A, sfNow, d.msgId, d.frag, FT_CUSTODY);
            break;
        }

        last_seen_msgId_a = d.msgId;
        last_seen_frag_a = d.frag;

        if (qPush(qAC, qACHead, qACCount, d)) {
            sendAck(NODE_A, sfNow, d.msgId, d.frag, FT_CUSTODY);
            ackA++;
            logRelay("A->C", d.msgId, d.frag, sfNow);
            Serial.printf("CUSTODY|B|A->C|msg=%u|frag=%u|Q=%u\n", d.msgId, d.frag, qACCount);
        }
        break;
    }
}

void receiveFromC() {
    uint8_t ch = hopChannel(sfNow, FHSS_SEED_BC, blacklist);
    tune(ch);
    uint32_t deadline = micros() + 9000;
    while ((int32_t)(micros() - deadline) < 0) {
        if (!radio.available()) {
            delayMicroseconds(50);
            continue;
        }
        uint8_t raw[32];
        radio.read(raw, 32);
        DataFrame d;
        memcpy(&d, raw, 32);
        if (d.magic != SP_MAGIC || d.version != SP_VERSION || d.type != FT_DATA || d.src != NODE_C || d.dst != NODE_B || d.len > DATA_PLAINTEXT_MAX) continue;

        rxC++;
        // Check for duplicate fragment
        if (d.msgId == last_seen_msgId_c && d.frag == last_seen_frag_c) {
            sendAck(NODE_C, sfNow, d.msgId, d.frag, FT_CUSTODY);
            break;
        }

        last_seen_msgId_c = d.msgId;
        last_seen_frag_c = d.frag;

        if (qPush(qCA, qCAHead, qCACount, d)) {
            sendAck(NODE_C, sfNow, d.msgId, d.frag, FT_CUSTODY);
            ackC++;
            logRelay("C->A", d.msgId, d.frag, sfNow);
            Serial.printf("CUSTODY|B|C->A|msg=%u|frag=%u|Q=%u\n", d.msgId, d.frag, qCACount);
        }
        break;
    }
}

bool forwardOne(QueueItem *q, volatile uint8_t &tail, volatile uint8_t count, uint8_t dst, uint32_t seed, bool forwardToC) {
    if (!count) return false;
    DataFrame f;
    if (!qPeek(q, tail, count, f)) return false;

    f.src = NODE_B;
    f.dst = dst;
    uint8_t ch = hopChannel(sfNow, seed, blacklist);
    tune(ch);

    bool ok = txFrame(radio, &f);
    if (!ok) return false;

    uint32_t end = micros() + 3000;
    bool delivered = false;
    while ((int32_t)(micros() - end) < 0) {
        if (!radio.available()) {
            delayMicroseconds(40);
            continue;
        }
        uint8_t raw[32];
        radio.read(raw, 32);
        AckFrame a;
        memcpy(&a, raw, 32);
        if (a.magic == SP_MAGIC && a.version == SP_VERSION && a.type == FT_DELIVERY && a.src == dst && a.dst == NODE_B && a.msgId == f.msgId && a.frag == f.frag) {
            delivered = true;
            break;
        }
    }

    if (delivered) {
        qPop(tail, count);
        if (forwardToC) txC++; else txA++;
        Serial.printf("DELIVER|B|%s|msg=%u|frag=%u|RemainingQ=%u\n", forwardToC ? "A->C" : "C->A", f.msgId, f.frag, count - 1);
    }
    return delivered;
}

// ---------------- Embedded Web Portal ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>HopperNet / SpectrumPipe — Node B (Master Relay)</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #070a13; color: #e2e8f0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; padding: 14px; }
  .container { max-width: 540px; margin: 0 auto; }
  .card { background: #111827; border: 1px solid #1f293d; border-radius: 12px; padding: 16px; margin-bottom: 12px; box-shadow: 0 4px 14px rgba(0,0,0,0.4); }
  .header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #1f293d; padding-bottom: 10px; margin-bottom: 12px; }
  .title { font-size: 17px; font-weight: 700; color: #f59e0b; letter-spacing: 0.5px; }
  .badge { font-size: 11px; font-weight: 700; padding: 4px 10px; border-radius: 20px; text-transform: uppercase; background: #065f46; color: #34d399; letter-spacing: 0.5px; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 12px; }
  .stat-box { background: #172033; padding: 10px 12px; border-radius: 8px; border: 1px solid #24324f; }
  .stat-label { font-size: 11px; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
  .stat-val { font-size: 16px; font-weight: 700; color: #f8fafc; font-family: monospace; margin-top: 2px; }
  .led-grid { display: grid; grid-template-columns: repeat(10, 1fr); gap: 6px; margin-top: 10px; }
  .led-pill { background: #131b2e; border: 1px solid #23304d; border-radius: 6px; padding: 6px 2px; text-align: center; font-size: 10px; font-family: monospace; transition: all 0.2s ease; }
  .led-pill.ok { background: rgba(16, 185, 129, 0.15); border-color: #10b981; color: #34d399; }
  .led-pill.bad { background: rgba(239, 68, 68, 0.18); border-color: #ef4444; color: #f87171; }
  .led-pill .hop-no { font-size: 8px; color: #6ee7b7; margin-bottom: 2px; }
  .rate-bar-bg { background: #1e293b; border-radius: 6px; height: 8px; overflow: hidden; margin-top: 6px; }
  .rate-bar-fill { background: #10b981; height: 100%; width: 100%; transition: width 0.3s ease; }
  .msg-list { list-style: none; max-height: 180px; overflow-y: auto; }
  .msg-item { background: #172033; border-left: 3px solid #f59e0b; padding: 8px 10px; margin-bottom: 6px; border-radius: 4px; font-size: 13px; display: flex; justify-content: space-between; }
  .msg-item.rev { border-left-color: #06b6d4; }
  .empty { color: #64748b; font-size: 13px; text-align: center; padding: 12px 0; }
  .live-dot { width: 8px; height: 8px; background: #f59e0b; border-radius: 50%; display: inline-block; margin-right: 6px; animation: pulse 1.5s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div>
        <div class="title">SPECTRUM-PIPE NODE B</div>
        <div style="font-size:12px;color:#94a3b8;margin-top:2px;">SSID: hopperb &bull; 192.168.4.1 &bull; 50ms Superframe</div>
      </div>
      <div class="badge">MASTER CLOCK</div>
    </div>
    
    <div class="grid">
      <div class="stat-box">
        <div class="stat-label">CURRENT CH / FREQ</div>
        <div class="stat-val" id="val-ch">CH --</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">SUPERFRAME (SF)</div>
        <div class="stat-val" id="val-sf">#0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">FWD CUSTODY (A&rarr;C)</div>
        <div class="stat-val" id="val-fwd-buf" style="color:#f59e0b;">0 pk</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">REV CUSTODY (C&rarr;A)</div>
        <div class="stat-val" id="val-rev-buf" style="color:#06b6d4;">0 pk</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">DELIVERED (A &rarr; C)</div>
        <div class="stat-val" id="val-fwd-del" style="color:#34d399;">0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">DYNAMIC BLACKLIST</div>
        <div class="stat-val" id="val-jam" style="color:#ef4444;">0 ch</div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="header" style="margin-bottom:6px;">
      <div style="font-size:13px;font-weight:700;color:#cbd5e1;"><span class="live-dot"></span>20-HOP SPECTRUM STREAM (1 SEC / 50ms SUPERFRAME)</div>
      <div id="sync-rate-badge" style="font-size:12px;font-weight:700;color:#34d399;font-family:monospace;">20 / 20 (100%)</div>
    </div>
    <div class="rate-bar-bg"><div id="rate-bar" class="rate-bar-fill" style="width:100%;"></div></div>
    <div id="leds-container" class="led-grid">
      <!-- 20 LEDs rendered here -->
    </div>
  </div>

  <div class="card">
    <div class="header" style="margin-bottom:8px;">
      <div style="font-size:13px;font-weight:600;color:#cbd5e1;">RELAY CUSTODY ROUTING LOG</div>
      <div id="relay-status" style="font-size:12px;color:#94a3b8;">Total Relayed: 0</div>
    </div>
    <div id="relay-feed" class="msg-list">
      <div class="empty">No packets routed yet.</div>
    </div>
  </div>
</div>

<script>
let lastHistoryCount = -1;

function initLeds() {
  const container = document.getElementById('leds-container');
  let html = '';
  for (let i = 0; i < 20; i++) {
    html += '<div class="led-pill ok" id="led-' + i + '"><div class="hop-no">#' + (i + 1) + '</div><div class="ch-no">CH--</div></div>';
  }
  container.innerHTML = html;
}
initLeds();

async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    const d = await res.json();
    
    document.getElementById('val-ch').textContent = 'CH ' + d.ch + ' (' + (2400 + d.ch) + 'M)';
    document.getElementById('val-sf').textContent = '#' + d.sf;
    document.getElementById('val-fwd-buf').textContent = d.fwd_buf + ' pk';
    document.getElementById('val-rev-buf').textContent = d.rev_buf + ' pk';
    document.getElementById('val-fwd-del').textContent = d.fwd_delivered + ' (rev: ' + d.rev_delivered + ')';
    document.getElementById('val-jam').textContent = d.jam_count + ' ch';
    document.getElementById('relay-status').textContent = 'Total Relayed: ' + (d.fwd_delivered + d.rev_delivered);

    if (d.recent_hops && d.recent_hops.length === 20) {
      let matchedCount = d.matched_sec || 20;
      let pct = Math.round((matchedCount / 20) * 100);
      document.getElementById('sync-rate-badge').textContent = matchedCount + ' / 20 (' + pct + '%)';
      document.getElementById('rate-bar').style.width = pct + '%';
      for (let i = 0; i < 20; i++) {
        const item = d.recent_hops[i];
        const el = document.getElementById('led-' + i);
        if (el) {
          el.className = 'led-pill ' + (item.ok ? 'ok' : 'bad');
          el.querySelector('.ch-no').textContent = 'CH ' + item.ch;
        }
      }
    }

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
    html += '<div class="msg-item ' + (isRev ? 'rev' : '') + '"><div><span style="font-size:10px;padding:2px 5px;background:#243044;border-radius:3px;margin-right:6px;">' + m.dir + '</span><strong>Msg #' + m.msgId + ' (frag ' + m.frag + ')</strong></div><div style="font-family:monospace;font-size:11px;color:#94a3b8;">SF #' + m.sf + '</div></div>';
  }
  feed.innerHTML = html;
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
    HopRecord snap_hops[HOPS_PER_SEC];
    uint8_t snap_matched = 0;

    portENTER_CRITICAL(&queueMux);
    snap_matched = display_matched_count;
    for (int i = 0; i < HOPS_PER_SEC; i++) {
        snap_hops[i] = display_hops[i];
    }
    portEXIT_CRITICAL(&queueMux);

    String json = "{";
    json += "\"node\":\"node_b\",";
    json += "\"ssid\":\"" NODE_B_SSID "\",";
    json += "\"ch\":" + String(currentChannel) + ",";
    json += "\"sf\":" + String(sfNow) + ",";
    json += "\"fwd_buf\":" + String(qACCount) + ",";
    json += "\"rev_buf\":" + String(qCACount) + ",";
    json += "\"jam_count\":" + String(blCount(blacklist)) + ",";
    json += "\"fwd_delivered\":" + String(txC) + ",";
    json += "\"rev_delivered\":" + String(txA) + ",";
    json += "\"matched_sec\":" + String(snap_matched) + ",";
    json += "\"hops_per_sec\":20,";
    
    // 20 Channels Per Second Spectrum Array
    json += "\"recent_hops\":[";
    for (int i = 0; i < HOPS_PER_SEC; i++) {
        if (i > 0) json += ",";
        json += "{\"ch\":" + String(snap_hops[i].channel) + ",\"ok\":" + String(snap_hops[i].matched) + "}";
    }
    json += "],";

    json += "\"history_count\":" + String(rh_count) + ",";
    json += "\"history\":[";
    
    int count = (rh_count < RELAY_LOG_MAX) ? rh_count : RELAY_LOG_MAX;
    int start = (rh_count < RELAY_LOG_MAX) ? 0 : (rh_count % RELAY_LOG_MAX);
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % RELAY_LOG_MAX;
        if (i > 0) json += ",";
        json += "{\"dir\":\"" + String(relay_history[idx].dir) + "\",";
        json += "\"msgId\":" + String(relay_history[idx].msgId) + ",";
        json += "\"frag\":" + String(relay_history[idx].frag) + ",";
        json += "\"sf\":" + String(relay_history[idx].sf) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

// ---------------- Background FreeRTOS Task (Core 0) ----------------
void backgroundTaskCore0(void *pvParameters) {
    uint32_t last_lcd_ms = 0;
    for (;;) {
        server.handleClient();

        if (lcd_available && (millis() - last_lcd_ms >= 250)) {
            last_lcd_ms = millis();
            lcd.setCursor(0, 0);
            lcd.print(F("CH:"));
            if (currentChannel < 100) lcd.print(F(" "));
            if (currentChannel < 10) lcd.print(F(" "));
            lcd.print(currentChannel);
            lcd.print(F(" SF:"));
            lcd.print(sfNow % 10000);

            lcd.setCursor(0, 1);
            lcd.print(F("F:"));
            lcd.print(qACCount);
            lcd.print(F(" R:"));
            lcd.print(qCACount);
            lcd.print(F(" J:"));
            lcd.print(blCount(blacklist));
            lcd.print(F(" "));
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(300);
    Serial.println(F("=========================================="));
    Serial.println(F("  SpectrumPipe NODE B — Relay (SSID: hopperb)"));
    Serial.println(F("=========================================="));

    // 1. SoftAP Setup (Channel 6, 75% Power)
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_15dBm);
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
        lcd.print(F("SpectrumPipe B"));
        lcd.setCursor(0, 1);
        lcd.print(F("Master Relay ON"));
        delay(500);
    }

    blClear(blacklist);
    memset(badStreak, 0, sizeof(badStreak));
    memset(goodStreak, 0, sizeof(goodStreak));

    for (int i = 0; i < HOPS_PER_SEC; i++) {
        sec_hops[i].channel = hopChannel(i, FHSS_SEED_AB, blacklist);
        sec_hops[i].matched = 1;
        display_hops[i] = sec_hops[i];
    }

    // 3. Initialize Radio
    radioCommonBegin(radio);
    Serial.println(F("[NODE_B] Mesh ready on Channel 0. Starting 50ms Superframe master clock..."));

    // 4. Start Core 0 Background Task
    xTaskCreatePinnedToCore(
        backgroundTaskCore0,
        "BgTaskCore0",
        4096,
        NULL,
        1,
        NULL,
        0
    );
}

// ---------------- Real-Time 50 ms Superframe Loop (Core 1) ----------------
void loop() {
    uint32_t now = micros();
    sfNow = now / SUPERFRAME_US;
    uint32_t phase = now % SUPERFRAME_US;

    // Slot 0 (0-4 ms): Broadcast Sync Beacon on Rendezvous Channel 0
    static uint32_t lastSF = 0xFFFFFFFFUL;
    if (sfNow != lastSF) {
        lastSF = sfNow;
        sendSync();

        uint8_t slot = (uint8_t)(sfNow % HOPS_PER_SEC);
        sec_hops[slot].channel = hopChannel(sfNow, FHSS_SEED_AB, blacklist);
        sec_hops[slot].matched = 1;

        if (slot == 0 && sfNow > last_sec_boundary_sf) {
            last_sec_boundary_sf = sfNow;
            portENTER_CRITICAL(&queueMux);
            for (int i = 0; i < HOPS_PER_SEC; i++) {
                display_hops[i] = sec_hops[i];
            }
            display_matched_count = HOPS_PER_SEC;
            portEXIT_CRITICAL(&queueMux);
        }
    }

    // Slot 1 (4-16 ms): A -> B Forward Path
    if (phase >= AB_RX_START && phase < BC_TX_START) {
        receiveFromA();
    }
    // Slot 2 (16-28 ms): B -> C Forward Drain
    else if (phase >= BC_TX_START && phase < BC_RX_START) {
        forwardOne(qAC, qACTail, qACCount, NODE_C, FHSS_SEED_BC, true);
    }
    // Slot 3 (28-40 ms): C -> B Return Path
    else if (phase >= BC_RX_START && phase < AB_TX_START) {
        receiveFromC();
    }
    // Slot 4 (40-48 ms): B -> A Return Drain
    else if (phase >= AB_TX_START && phase < GUARD_START) {
        forwardOne(qCA, qCATail, qCACount, NODE_A, FHSS_SEED_AB, false);
    }
    // Slot 5 (48-50 ms): Guard / RPD Probe (Energy observation only)
    else if (phase >= GUARD_START) {
        static uint32_t lastProbeSF = 0;
        if (sfNow - lastProbeSF >= 10) {
            lastProbeSF = sfNow;
            uint8_t probe = (uint8_t)(RF_CHANNEL_FIRST + (mix32(sfNow * 0x9E37u) % RF_CHANNEL_COUNT));
            tune(probe);
            delayMicroseconds(100);
            if (radio.testCarrier()) {
                scoreJammerEnergy(probe);
            } else {
                scoreSuccess(probe);
            }
        }

        // Periodic Blacklist Health Check (every 10 seconds / 200 SF): decay bad streaks
        static uint32_t lastDecaySF = 0;
        if (sfNow - lastDecaySF >= 200) {
            lastDecaySF = sfNow;
            for (uint8_t ch = RF_CHANNEL_FIRST; ch <= RF_CHANNEL_LAST; ch++) {
                uint8_t i = ch - RF_CHANNEL_FIRST;
                if (badStreak[i] > 0) badStreak[i]--;
            }
        }
    }

    delayMicroseconds(50);
}
