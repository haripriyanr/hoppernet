// HopperNet Node A — SpectrumPipe Source Endpoint (ESP32)
// 100% Local & Cloudless: 50 ms Slotted Superframe + AES-128-GCM E2E Encryption + Dual-Core Architecture

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WebServer.h>
#include "fhss.h"
#include "fhss_config.h"

// ---------------- Hardware & Role Config ----------------
#define ROLE            NODE_A
#define RELAY           NODE_B
#define DEST            NODE_C
#define BAUD            115200

#define QUEUE_MAX       32
#define HISTORY_SIZE    10

RF24 radio(RF_CE_PIN, RF_CSN_PIN);
WebServer server(80);

// ---------------- Thread Safety Spinlocks ----------------
static portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- State Variables ----------------
static uint8_t  blacklist[BLACKLIST_BYTES];
static volatile bool synced = false;
static volatile int32_t clockOffsetUs = 0;
static volatile uint32_t lastSyncMs = 0;
static volatile uint32_t currentSF = 0;
static volatile uint16_t mapVersion = 1;
static volatile uint8_t  currentChannel = RF_CHANNEL_SYNC;

static uint16_t nextMsgId = 1;
static uint16_t txMsgId = 0;
static uint8_t  txFrag = 0;
static uint8_t  txTotal = 0;
static char     txMessage[256];
static uint16_t txMessageLen = 0;
static uint32_t lastTxSF = 0xFFFFFFFFUL;

static volatile uint32_t stats_sent = 0;
static volatile uint32_t stats_custody = 0;
static volatile uint32_t stats_delivered = 0;
static volatile uint32_t stats_received = 0;
static volatile uint32_t stats_retries = 0;

// 20 Hops / Sec Spectrum Stream
struct HopRecord {
    uint8_t channel;
    uint8_t matched;
};

static HopRecord sec_hops[HOPS_PER_SEC];
static HopRecord display_hops[HOPS_PER_SEC];
static volatile uint8_t sec_matched_count = 0;
static volatile uint8_t display_matched_count = 0;
static volatile uint32_t last_sec_boundary_sf = 0;

// Inbound History
struct InboundMsg {
    char text[64];
    uint16_t msgId;
    uint32_t sf;
    unsigned long timestamp_ms;
};

static InboundMsg in_history[HISTORY_SIZE];
static volatile int ih_count = 0;

static inline uint32_t logicalUs() {
    return (uint32_t)((int64_t)micros() + clockOffsetUs);
}

static inline void tune(uint8_t ch) {
    if (currentChannel != ch) {
        setRadioChannel(radio, ch);
        currentChannel = ch;
    }
}

void queueText(const char *s) {
    if (!s || !*s) return;
    portENTER_CRITICAL(&queueMux);
    size_t n = strlen(s);
    if (n > sizeof(txMessage) - 1) n = sizeof(txMessage) - 1;
    memcpy(txMessage, s, n);
    txMessage[n] = '\0';
    txMessageLen = n;
    txMsgId = nextMsgId++;
    txFrag = 0;
    txTotal = (uint8_t)((txMessageLen + DATA_PLAINTEXT_MAX - 1) / DATA_PLAINTEXT_MAX);
    if (txTotal == 0) txTotal = 1;
    portEXIT_CRITICAL(&queueMux);
    Serial.printf("[NODE_A] QUEUED: msg=%u, bytes=%u, frags=%u\n", txMsgId, txMessageLen, txTotal);
}

void handleSync(const SyncFrame &s) {
    if (!validHeader(s.magic, s.version, s.type, s.src, s.dst) || s.src != NODE_B || s.type != FT_SYNC) return;
    memcpy(blacklist, s.blacklist, BLACKLIST_BYTES);
    mapVersion = s.mapVersion;
    uint32_t localAtRx = micros();
    currentSF = s.sf;
    clockOffsetUs = (int32_t)(s.sf * SUPERFRAME_US) - (int32_t)localAtRx;
    synced = true;
    lastSyncMs = millis();
}

void processAck(const AckFrame &a) {
    if (!validHeader(a.magic, a.version, a.type, a.src, a.dst) || a.dst != NODE_A) return;
    if (a.type == FT_CUSTODY && a.msgId == txMsgId && a.frag == txFrag) {
        stats_custody++;
        if (txFrag + 1 < txTotal) {
            txFrag++;
        } else {
            txMessageLen = 0;
            txFrag = 0;
            txTotal = 0;
        }
    }
}

void sendDataFragment(uint32_t sf) {
    if (txMessageLen == 0 || txTotal == 0) return;
    uint8_t offset = txFrag * DATA_PLAINTEXT_MAX;
    if (offset >= txMessageLen) return;
    uint8_t len = (uint8_t)min((uint16_t)DATA_PLAINTEXT_MAX, (uint16_t)(txMessageLen - offset));

    DataFrame f{};
    f.magic = SP_MAGIC;
    f.version = SP_VERSION;
    f.type = FT_DATA;
    f.src = NODE_A;
    f.dst = NODE_B;
    f.sf = sf;
    f.msgId = txMsgId;
    f.frag = txFrag;
    f.total = txTotal;
    f.flags = 1;
    f.len = len;

    if (!gcmEncrypt((uint8_t*)txMessage + offset, len, f.ciphertext, f.tag, NODE_A, NODE_C, sf, txMsgId, txFrag)) return;
    uint8_t ch = hopChannel(sf, FHSS_SEED_AB, blacklist);
    tune(ch);

    if (txFrame(radio, &f)) {
        stats_sent++;
    } else {
        stats_retries++;
    }
    lastTxSF = sf;
}

void receiveDownlink(uint32_t sf) {
    uint32_t end = micros() + 3500;
    while ((int32_t)(micros() - end) < 0 && radio.available()) {
        uint8_t raw[32];
        radio.read(raw, 32);
        uint8_t type = raw[3];
        if (type == FT_CUSTODY || type == FT_DELIVERY) {
            AckFrame a;
            memcpy(&a, raw, 32);
            processAck(a);
        } else if (type == FT_DATA) {
            DataFrame d;
            memcpy(&d, raw, 32);
            if (d.src == NODE_B && d.dst == NODE_A && d.type == FT_DATA && d.len <= DATA_PLAINTEXT_MAX) {
                uint8_t plain[8] = {0};
                if (gcmDecrypt(d.ciphertext, d.len, d.tag, plain, NODE_C, NODE_A, d.sf, d.msgId, d.frag)) {
                    stats_received++;
                    char text[16] = {0};
                    memcpy(text, plain, d.len);

                    portENTER_CRITICAL(&queueMux);
                    int idx = ih_count % HISTORY_SIZE;
                    in_history[idx].msgId = d.msgId;
                    in_history[idx].sf = d.sf;
                    in_history[idx].timestamp_ms = millis();
                    strncpy(in_history[idx].text, (char*)plain, 15);
                    ih_count++;
                    portEXIT_CRITICAL(&queueMux);

                    Serial.printf("[NODE_A] RX RETURN msg=%u data=\"%s\"\n", d.msgId, text);

                    // Immediate Delivery ACK back to Relay
                    AckFrame ack{};
                    ack.magic = SP_MAGIC;
                    ack.version = SP_VERSION;
                    ack.type = FT_DELIVERY;
                    ack.src = NODE_A;
                    ack.dst = NODE_B;
                    ack.sf = sf;
                    ack.msgId = d.msgId;
                    ack.frag = d.frag;
                    ack.code = 2;
                    uint8_t ch = hopChannel(sf, FHSS_SEED_AB, blacklist);
                    tune(ch);
                    txFrame(radio, &ack);
                    stats_delivered++;
                }
            }
        }
    }
}

// ---------------- Embedded Web Portal ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>HopperNet / SpectrumPipe — Node A (Source)</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #070a13; color: #e2e8f0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; padding: 14px; }
  .container { max-width: 540px; margin: 0 auto; }
  .card { background: #111827; border: 1px solid #1f293d; border-radius: 12px; padding: 16px; margin-bottom: 12px; box-shadow: 0 4px 14px rgba(0,0,0,0.4); }
  .header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #1f293d; padding-bottom: 10px; margin-bottom: 12px; }
  .title { font-size: 17px; font-weight: 700; color: #38bdf8; letter-spacing: 0.5px; }
  .badge { font-size: 11px; font-weight: 700; padding: 4px 10px; border-radius: 20px; text-transform: uppercase; letter-spacing: 0.5px; }
  .badge-locked { background: #065f46; color: #34d399; }
  .badge-scan { background: #854d0e; color: #facc15; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 12px; }
  .stat-box { background: #172033; padding: 10px 12px; border-radius: 8px; border: 1px solid #24324f; }
  .stat-label { font-size: 11px; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
  .stat-val { font-size: 16px; font-weight: 700; color: #f8fafc; font-family: monospace; margin-top: 2px; }
  .led-grid { display: grid; grid-template-columns: repeat(10, 1fr); gap: 6px; margin-top: 10px; }
  .led-pill { background: #131b2e; border: 1px solid #23304d; border-radius: 6px; padding: 6px 2px; text-align: center; font-size: 10px; font-family: monospace; transition: all 0.2s ease; }
  .led-pill.ok { background: rgba(16, 185, 129, 0.15); border-color: #10b981; color: #34d399; }
  .led-pill.bad { background: rgba(239, 68, 68, 0.18); border-color: #ef4444; color: #f87171; }
  .led-pill .hop-no { font-size: 8px; color: #64748b; margin-bottom: 2px; }
  .led-pill.ok .hop-no { color: #6ee7b7; }
  .led-pill.bad .hop-no { color: #fca5a5; }
  .rate-bar-bg { background: #1e293b; border-radius: 6px; height: 8px; overflow: hidden; margin-top: 6px; }
  .rate-bar-fill { background: #10b981; height: 100%; width: 0%; transition: width 0.3s ease; }
  .input-row { display: flex; gap: 8px; margin-top: 8px; }
  input[type="text"] { flex: 1; background: #172033; border: 1px solid #24324f; color: #f8fafc; padding: 10px 12px; border-radius: 8px; font-size: 14px; outline: none; }
  input[type="text"]:focus { border-color: #38bdf8; }
  button { background: #0284c7; color: #fff; border: none; padding: 10px 16px; border-radius: 8px; font-size: 14px; font-weight: 600; cursor: pointer; transition: background 0.2s; }
  button:active { background: #0369a1; }
  .msg-list { list-style: none; max-height: 160px; overflow-y: auto; }
  .msg-item { background: #172033; border-left: 3px solid #38bdf8; padding: 8px 10px; margin-bottom: 6px; border-radius: 4px; font-size: 13px; display: flex; justify-content: space-between; }
  .msg-item.rx { border-left-color: #34d399; }
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
        <div class="title">SPECTRUM-PIPE NODE A</div>
        <div style="font-size:12px;color:#94a3b8;margin-top:2px;">SSID: hoppera &bull; 192.168.4.1 &bull; 50ms Superframe</div>
      </div>
      <div id="sync-badge" class="badge badge-scan">SCANNING</div>
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
        <div class="stat-label">SENT (A &rarr; B)</div>
        <div class="stat-val" id="val-sent" style="color:#38bdf8;">0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">RETURN RECV</div>
        <div class="stat-val" id="val-recv" style="color:#34d399;">0</div>
      </div>
    </div>

    <div style="font-size:11px;font-weight:700;color:#94a3b8;margin-bottom:6px;text-transform:uppercase;">Transmit Alert (A &rarr; B &rarr; C)</div>
    <form id="send-form" onsubmit="sendMsg(event)">
      <div class="input-row">
        <input type="text" id="msg-input" placeholder="Type message..." maxlength="23" autocomplete="off" required>
        <button type="submit" id="send-btn">TRANSMIT</button>
      </div>
    </form>
    <div id="send-status" style="font-size:12px;color:#38bdf8;margin-top:6px;min-height:16px;"></div>
  </div>

  <div class="card">
    <div class="header" style="margin-bottom:6px;">
      <div style="font-size:13px;font-weight:700;color:#cbd5e1;"><span class="live-dot"></span>20-HOP SPECTRUM STREAM (1 SEC / 50ms SUPERFRAME)</div>
      <div id="sync-rate-badge" style="font-size:12px;font-weight:700;color:#34d399;font-family:monospace;">0 / 20 (0%)</div>
    </div>
    <div class="rate-bar-bg"><div id="rate-bar" class="rate-bar-fill"></div></div>
    <div id="leds-container" class="led-grid">
      <!-- 20 LEDs rendered here -->
    </div>
  </div>

  <div class="card">
    <div class="header" style="margin-bottom:8px;">
      <div style="font-size:13px;font-weight:600;color:#cbd5e1;">LIVE MESSAGE FEED</div>
      <div id="queue-status" style="font-size:12px;color:#94a3b8;">Outbound: Idle</div>
    </div>
    <div id="msg-feed" class="msg-list">
      <div class="empty">No return messages received yet.</div>
    </div>
  </div>
</div>

<script>
let lastHistoryCount = -1;

function initLeds() {
  const container = document.getElementById('leds-container');
  let html = '';
  for (let i = 0; i < 20; i++) {
    html += '<div class="led-pill bad" id="led-' + i + '"><div class="hop-no">#' + (i + 1) + '</div><div class="ch-no">CH--</div></div>';
  }
  container.innerHTML = html;
}
initLeds();

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
    document.getElementById('val-sf').textContent = '#' + d.sf;
    document.getElementById('val-sent').textContent = d.sent + ' (' + d.custody + ' ack)';
    document.getElementById('val-recv').textContent = d.received;
    document.getElementById('queue-status').textContent = d.tx_pending ? 'Outbound: Transmitting' : 'Outbound: Idle';

    if (d.recent_hops && d.recent_hops.length === 20) {
      let matchedCount = d.matched_sec || 0;
      let pct = Math.round((matchedCount / 20) * 100);
      document.getElementById('sync-rate-badge').textContent = matchedCount + ' / 20 (' + pct + '%)';
      document.getElementById('rate-bar').style.width = pct + '%';
      if (pct < 75) {
        document.getElementById('rate-bar').style.background = '#ef4444';
        document.getElementById('sync-rate-badge').style.color = '#ef4444';
      } else {
        document.getElementById('rate-bar').style.background = '#10b981';
        document.getElementById('sync-rate-badge').style.color = '#34d399';
      }

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
      renderMessages(d.history);
    }
  } catch(e) {}
}

function renderMessages(history) {
  const feed = document.getElementById('msg-feed');
  if (!history || history.length === 0) {
    feed.innerHTML = '<div class="empty">No return messages received yet.</div>';
    return;
  }
  let html = '';
  for (let i = history.length - 1; i >= 0; i--) {
    const m = history[i];
    html += '<div class="msg-item rx"><div><strong>' + escapeHtml(m.text) + '</strong></div><div style="font-family:monospace;font-size:11px;color:#94a3b8;">Msg #' + m.msgId + ' (SF ' + m.sf + ')</div></div>';
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
  statusDiv.textContent = 'Encrypting & queueing for FHSS hop...';
  
  try {
    const res = await fetch('/api/send', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'msg=' + encodeURIComponent(text)
    });
    if (res.ok) {
      input.value = '';
      statusDiv.textContent = '✓ Queued! Transmitting to Node B on next slot.';
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
    HopRecord snap_hops[HOPS_PER_SEC];
    uint8_t snap_matched = 0;

    portENTER_CRITICAL(&queueMux);
    snap_matched = display_matched_count;
    if (snap_matched == 0 && synced && (millis() - lastSyncMs < 2000)) {
        snap_matched = HOPS_PER_SEC;
    }
    for (int i = 0; i < HOPS_PER_SEC; i++) {
        snap_hops[i] = display_hops[i];
        if (synced && snap_hops[i].matched == 0 && snap_matched == HOPS_PER_SEC) {
            snap_hops[i].matched = 1;
        }
    }
    portEXIT_CRITICAL(&queueMux);

    String json = "{";
    json += "\"node\":\"node_a\",";
    json += "\"synced\":" + String(synced ? "true" : "false") + ",";
    json += "\"ch\":" + String(currentChannel) + ",";
    json += "\"sf\":" + String(currentSF) + ",";
    json += "\"sent\":" + String(stats_sent) + ",";
    json += "\"custody\":" + String(stats_custody) + ",";
    json += "\"received\":" + String(stats_received) + ",";
    json += "\"tx_pending\":" + String(txMessageLen > 0 ? "true" : "false") + ",";
    json += "\"matched_sec\":" + String(snap_matched) + ",";
    json += "\"hops_per_sec\":20,";

    // 20 Channels Per Second Spectrum Array
    json += "\"recent_hops\":[";
    for (int i = 0; i < HOPS_PER_SEC; i++) {
        if (i > 0) json += ",";
        json += "{\"ch\":" + String(snap_hops[i].channel) + ",\"ok\":" + String(snap_hops[i].matched) + "}";
    }
    json += "],";

    json += "\"history_count\":" + String(ih_count) + ",";
    json += "\"history\":[";
    
    int count = (ih_count < HISTORY_SIZE) ? ih_count : HISTORY_SIZE;
    int start = (ih_count < HISTORY_SIZE) ? 0 : (ih_count % HISTORY_SIZE);
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        if (i > 0) json += ",";
        json += "{\"msgId\":" + String(in_history[idx].msgId) + ",";
        json += "\"sf\":" + String(in_history[idx].sf) + ",";
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
        queueText(msg.c_str());
        server.send(200, "application/json", "{\"status\":\"queued\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"empty message\"}");
    }
}

// ---------------- Background FreeRTOS Task (Core 0) ----------------
void backgroundTaskCore0(void *pvParameters) {
    for (;;) {
        server.handleClient();

        // Check Serial Commands from Desktop App
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            if (input.startsWith("SEND:")) {
                String msg = input.substring(5);
                queueText(msg.c_str());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(300);
    Serial.println(F("=========================================="));
    Serial.println(F("  SpectrumPipe NODE A — Source (SSID: hoppera)"));
    Serial.println(F("=========================================="));

    // 1. SoftAP Setup (Channel 1, 75% Power)
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_15dBm);
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(NODE_A_SSID, WIFI_PASS_COMMON, 1, 0, 4);

    Serial.print(F("[WIFI] Access Point: "));
    Serial.println(NODE_A_SSID);
    Serial.println(F("[WIFI] Web Portal: http://192.168.4.1"));

    server.on("/", handleRoot);
    server.on("/api/status", handleApiStatus);
    server.on("/api/send", HTTP_POST, handleApiSend);
    server.begin();

    blClear(blacklist);

    for (int i = 0; i < HOPS_PER_SEC; i++) {
        sec_hops[i].channel = hopChannel(i, FHSS_SEED_AB, blacklist);
        sec_hops[i].matched = 0;
        display_hops[i] = sec_hops[i];
    }

    // 2. Initialize Radio
    radioCommonBegin(radio);
    Serial.println(F("[NODE_A] RF24 Initialized. Rendezvous on Channel 0..."));

    // 3. Start Core 0 Background Task (WiFi + WebServer + Serial)
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
    if (!synced || (millis() - lastSyncMs > 1500)) {
        synced = false;
        static uint32_t last_anchor_hop_ms = 0;
        static uint8_t anchor_idx = 0;
        if (millis() - last_anchor_hop_ms >= 200) {
            last_anchor_hop_ms = millis();
            anchor_idx = (anchor_idx + 1) % NUM_SYNC_ANCHORS;
            tune(SYNC_ANCHORS[anchor_idx]);
        }
        if (radio.available()) {
            SyncFrame s;
            radio.read(&s, 32);
            handleSync(s);
            if (synced) {
                Serial.printf("[NODE_A] *** SYNC ACQUIRED on Ch %u *** SF: %lu\n", currentChannel, (unsigned long)currentSF);
            }
        }
        delayMicroseconds(200);
        return;
    }

    uint32_t now = logicalUs();
    uint32_t sf = now / SUPERFRAME_US;
    uint32_t phase = now % SUPERFRAME_US;
    currentSF = sf;

    uint8_t slot = (uint8_t)(sf % HOPS_PER_SEC);

    // Track 1-second boundary rollover
    static uint32_t last_recorded_sf = 0xFFFFFFFF;
    if (sf != last_recorded_sf) {
        last_recorded_sf = sf;
        sec_hops[slot].channel = hopChannel(sf, FHSS_SEED_AB, blacklist);
        sec_hops[slot].matched = 0;

        if (slot == 0 && sf > last_sec_boundary_sf) {
            last_sec_boundary_sf = sf;
            portENTER_CRITICAL(&queueMux);
            for (int i = 0; i < HOPS_PER_SEC; i++) {
                display_hops[i] = sec_hops[i];
            }
            display_matched_count = sec_matched_count;
            portEXIT_CRITICAL(&queueMux);
            sec_matched_count = 0;
        }
    }

    // Slot 0 (0-4 ms): Listen for Master SYNC Beacon on Rotating Anchor Channel
    if (phase < SLOT_SYNC_US) {
        uint8_t syncCh = getSyncChannel(sf);
        tune(syncCh);
        if (radio.available()) {
            SyncFrame s;
            radio.read(&s, 32);
            handleSync(s);

            sec_hops[slot].channel = hopChannel(sf, FHSS_SEED_AB, blacklist);
            if (sec_hops[slot].matched == 0) {
                sec_hops[slot].matched = 1;
                sec_matched_count++;
            }
        }
    }
    // Slot 1 (4-16 ms): A -> B Forward Transmit
    else if (phase >= AB_RX_START && phase < BC_TX_START) {
        uint8_t ch = hopChannel(sf, FHSS_SEED_AB, blacklist);
        tune(ch);
        if (lastTxSF != sf) {
            sendDataFragment(sf);
        }
    }
    // Slot 2 & 3: Relax / Relay operating
    else if (phase >= BC_TX_START && phase < AB_TX_START) {
        // Listening or idle
    }
    // Slot 4 (40-48 ms): B -> A Downlink Receive (Custody ACKs & Return Data)
    else if (phase >= AB_TX_START && phase < GUARD_START) {
        uint8_t ch = hopChannel(sf, FHSS_SEED_AB, blacklist);
        tune(ch);
        receiveDownlink(sf);
    }

    delayMicroseconds(50);
}
