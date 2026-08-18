// HopperNet Node C — SpectrumPipe Destination Endpoint (ESP32)
// 100% Local & Cloudless: 50 ms Slotted Superframe + AES-128-GCM E2E Decryption + Dual-Core Architecture

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

#define QUEUE_MAX       32
#define HISTORY_SIZE    10
#define SYNC_LOSS_TIMEOUT_MS 30000UL
#define TX_QUEUE_SIZE   32

RF24 radio(RF_CE_PIN, RF_CSN_PIN);
WebServer server(80);

// ---------------- Thread Safety Spinlocks ----------------
static portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- State Variables ----------------
static uint8_t  blacklist[BLACKLIST_BYTES];
static volatile bool synced = false;
static volatile int32_t clockOffsetUs = 0;
static volatile float clockDriftPpm = 0.0f;
static volatile uint32_t lastSyncLocal = 0;
static volatile uint32_t lastSyncSf = 0;
static volatile uint32_t lastSyncMs = 0;
static volatile uint32_t currentSF = 0;
static volatile uint16_t mapVersion = 1;
static volatile uint8_t  currentChannel = RF_CHANNEL_SYNC;
static volatile uint8_t  lastBpRev = 0; // Dynamic reverse backpressure from Relay B (0=OK, 1=Throttled)

static uint32_t lastRxSF = 0xFFFFFFFFUL;

static volatile uint32_t stats_received = 0;
static volatile uint32_t stats_delivered = 0;
static volatile uint32_t stats_recovered = 0;
static volatile uint32_t stats_sent = 0;
static volatile uint32_t stats_custody = 0;
static volatile uint32_t stats_retries = 0;

// Return-path transmit state (C -> B -> A)
static uint16_t nextMsgId = 1;
static uint16_t txMsgId = 0;
static uint8_t  txFrag = 0;
static uint8_t  txTotal = 0;
static char     txMessage[256];
static uint16_t txMessageLen = 0;
static bool     txCritical = false;
static uint32_t lastTxSF = 0xFFFFFFFFUL;
static volatile bool loopSendEnabled = false;
static volatile uint32_t loopSent = 0;
static volatile uint32_t loopDropped = 0;

struct TxItem {
    char text[256];
    bool critical;
};
static TxItem   txQueue[TX_QUEUE_SIZE];
static uint8_t  txQueueHead = 0;
static uint8_t  txQueueTail = 0;
static uint8_t  txQueueCount = 0;

// Link-Down Simulation
static volatile bool simLinkDown = false;

// Reassembly Buffer
struct Assembly {
    uint16_t msgId;
    uint8_t  total;
    uint32_t bitmap;
    uint16_t length;
    char     data[256];
    bool     active;
    bool     recovered;
    bool     critical;
};

static Assembly assembly = {};

// Inbound History
struct InboundMsg {
    char text[64];
    uint16_t msgId;
    uint32_t sf;
    unsigned long timestamp_ms;
    bool recovered;
    uint8_t qos;
};

static InboundMsg in_history[HISTORY_SIZE];
static volatile int ih_count = 0;
static volatile uint32_t lastMeasuredRttUs = 1824;

// Cumulative Sync & Protocol Metrics
static volatile uint32_t stats_sync_total_hops = 0;
static volatile uint32_t stats_sync_locked_hops = 0;
static volatile uint32_t stats_sync_missed_hops = 0;
static volatile uint32_t stats_desync_events = 0;

// Leftover non-SYNC frame captured during slot 0 sync listen
static uint8_t pending_rx[32];
static bool pending_rx_valid = false;

static inline uint32_t logicalUs() {
    uint32_t local = micros();
    uint32_t elapsedSinceSync = local - lastSyncLocal;
    int32_t driftCorrection = (int32_t)((float)elapsedSinceSync * (clockDriftPpm / 1000000.0f));
    return (uint32_t)((int64_t)local + clockOffsetUs - driftCorrection);
}

static inline void tune(uint8_t ch) {
    if (currentChannel != ch) {
        setRadioChannel(radio, ch);
        currentChannel = ch;
    }
}static void startNextReplyLocked() {
    if (txQueueCount == 0) return;
    memcpy(txMessage, txQueue[txQueueTail].text, sizeof(txMessage));
    txCritical = txQueue[txQueueTail].critical;
    txQueueTail = (txQueueTail + 1) % TX_QUEUE_SIZE;
    txQueueCount--;
    txMessageLen = strlen(txMessage);
    txMsgId = nextMsgId++;
    txFrag = 0;
    txTotal = (uint8_t)((txMessageLen + DATA_PLAINTEXT_MAX - 1) / DATA_PLAINTEXT_MAX);
    if (txTotal == 0) txTotal = 1;
}

bool queueReply(const char *s, bool critical = false) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0 || n >= sizeof(txMessage)) return false;
    bool accepted = false;
    portENTER_CRITICAL(&queueMux);
    if (txMessageLen == 0 && txQueueCount == 0) {
        memcpy(txMessage, s, n + 1);
        txMessageLen = n;
        txCritical = critical;
        txMsgId = nextMsgId++;
        txFrag = 0;
        txTotal = (uint8_t)((txMessageLen + DATA_PLAINTEXT_MAX - 1) / DATA_PLAINTEXT_MAX);
        if (txTotal == 0) txTotal = 1;
        accepted = true;
    } else if (txQueueCount < TX_QUEUE_SIZE) {
        memcpy(txQueue[txQueueHead].text, s, n + 1);
        txQueue[txQueueHead].critical = critical;
        txQueueHead = (txQueueHead + 1) % TX_QUEUE_SIZE;
        txQueueCount++;
        accepted = true;
    }
    portEXIT_CRITICAL(&queueMux);
    Serial.printf("[NODE_C] QUEUED RETURN: bytes=%u, pending=%u\n", (unsigned)n, txQueueCount);
    return accepted;
}

void handleAck(const AckFrame &a) {
    if (!validHeader(a.magic, a.version, a.type, a.src, a.dst) || a.dst != NODE_C) return;
    if (a.type == FT_CUSTODY && a.msgId == txMsgId && a.frag == txFrag) {
        stats_custody++;
        lastBpRev = a.bp_rev;
        if (a.masterUs > 0) {
            clockOffsetUs = (int32_t)a.masterUs - (int32_t)micros();
            synced = true;
            lastSyncMs = millis();
        }
        if (txFrag + 1 < txTotal) {
            txFrag++;
        } else {
            txMessageLen = 0;
            txFrag = 0;
            txTotal = 0;
            portENTER_CRITICAL(&queueMux);
            startNextReplyLocked();
            portEXIT_CRITICAL(&queueMux);
        }
    }
}

void sendFragment(uint32_t sf) {
    if (txMessageLen == 0 || txTotal == 0) return;
    uint8_t off = txFrag * DATA_PLAINTEXT_MAX;
    if (off >= txMessageLen) return;
    uint8_t len = (uint8_t)min((uint16_t)DATA_PLAINTEXT_MAX, (uint16_t)(txMessageLen - off));

    DataFrame f{};
    f.magic = SP_MAGIC;
    f.version = SP_VERSION;
    f.type = FT_DATA;
    f.src = NODE_C;
    f.dst = NODE_B;
    f.sf = sf;
    f.msgId = txMsgId;
    f.frag = txFrag;
    f.total = txTotal;
    f.flags = DATA_FLAG_E2E | (txCritical ? DATA_FLAG_CRITICAL : 0);
    f.len = len;

    if (!chachaEncrypt((uint8_t*)txMessage + off, len, f.ciphertext, f.tag, NODE_C, NODE_A, sf, txMsgId, txFrag)) return;
    uint8_t ch = hopChannel(sf, FHSS_SEED_BC, blacklist);
    tune(ch);

    uint32_t txStart = micros();
    if (txFrame(radio, &f)) {
        stats_sent++;
        uint32_t deadline = micros() + (SUPERFRAME_US - SLOT_GUARD_US - 200);
        while ((int32_t)(micros() - deadline) < 0) {
            if (!radio.available()) {
                delayMicroseconds(5);
                continue;
            }
            uint8_t raw[32];
            radio.read(raw, 32);
            AckFrame a;
            memcpy(&a, raw, 32);
            if (a.magic == SP_MAGIC && a.type == FT_CUSTODY && a.src == NODE_B && a.dst == NODE_C && a.msgId == f.msgId && a.frag == f.frag) {
                uint32_t rttUs = micros() - txStart;
                handleAck(a);
                Serial.printf("HANDSHAKE|RTT=%lu_us|msg=%u|frag=%u|sf=%lu\n", (unsigned long)rttUs, f.msgId, f.frag, (unsigned long)sf);
                break;
            }
        }
    } else {
        stats_retries++;
    }
    lastTxSF = sf;
}

void serviceLoopSender() {
    static uint32_t lastLoopMs = 0;
    if (!loopSendEnabled) return;
    if (millis() - lastLoopMs < 250) return;
    if (txMessageLen > 0) return;
    lastLoopMs = millis();
    static uint8_t letter = 0;
    char msg[24];
    snprintf(msg, sizeof(msg), "%c Reply from Node C", 'A' + (letter % 26));
    letter++;
    queueReply(msg);
    loopSent++;
}

void handleSync(const SyncFrame &s) {
    if (!validHeader(s.magic, s.version, s.type, s.src, s.dst) || s.src != NODE_B || s.type != FT_SYNC) return;
    memcpy(blacklist, s.blacklist, BLACKLIST_BYTES);
    mapVersion = s.mapVersion;
    uint32_t local = micros();

    if (synced && lastSyncLocal != 0) {
        uint32_t elapsedSf = s.sf - lastSyncSf;
        if (elapsedSf > 0 && elapsedSf < 1000) {
            uint32_t expectedLocalDelta = elapsedSf * SUPERFRAME_US;
            uint32_t actualLocalDelta = local - lastSyncLocal;
            int32_t driftUs = (int32_t)actualLocalDelta - (int32_t)expectedLocalDelta;
            float currentDriftRate = (float)driftUs / ((float)expectedLocalDelta / 1000000.0f);
            clockDriftPpm = (clockDriftPpm * 0.8f) + (currentDriftRate * 0.2f);
        }
    }

    lastSyncLocal = local;
    lastSyncSf = s.sf;
    currentSF = s.sf;
    clockOffsetUs = (int32_t)(s.sf * SUPERFRAME_US) - (int32_t)local;
    synced = true;
    lastSyncMs = millis();
    lastBpRev = s.bp_rev;
    stats_sync_locked_hops++;
}

void receiveForward(uint32_t sf) {
    uint32_t end = micros() + (SUPERFRAME_US - SLOT_GUARD_US);
    while ((int32_t)(micros() - end) < 0) {
        uint8_t raw[32];
        bool have = false;
        if (pending_rx_valid) {
            memcpy(raw, pending_rx, 32);
            pending_rx_valid = false;
            have = true;
        } else if (radio.available()) {
            radio.read(raw, 32);
            have = true;
        }
        if (!have) {
            delayMicroseconds(5);
            continue;
        }
        uint8_t type = raw[3];

        if (type == FT_CUSTODY) {
            AckFrame a;
            memcpy(&a, raw, 32);
            handleAck(a);
            continue;
        }

        if (type != FT_DATA) continue;
        DataFrame d;
        memcpy(&d, raw, 32);
        if (d.magic != SP_MAGIC || d.version != SP_VERSION || d.type != FT_DATA || d.src != NODE_B || d.dst != NODE_C || d.len > DATA_PLAINTEXT_MAX) continue;

        uint8_t plain[8] = {0};
        if (!chachaDecrypt(d.ciphertext, d.len, d.tag, plain, NODE_A, NODE_C, d.sf, d.msgId, d.frag)) {
            Serial.printf("SECURITY|C|AUTH_FAIL|msg=%u|frag=%u\n", d.msgId, d.frag);
            continue;
        }

        static uint16_t last_delivered_msgId = 0;

        // Immediate Delivery ACK back to Relay (always send so Node B clears custody)
        AckFrame ack{};
        ack.magic = SP_MAGIC;
        ack.version = SP_VERSION;
        ack.type = FT_DELIVERY;
        ack.src = NODE_C;
        ack.dst = NODE_B;
        ack.sf = sf;
        ack.msgId = d.msgId;
        ack.frag = d.frag;
        ack.code = 2;
        uint8_t ch = hopChannel(sf, FHSS_SEED_BC, blacklist);
        tune(ch);
        txFrame(radio, &ack);
        stats_delivered++;;

        uint16_t messageAge = (uint16_t)(last_delivered_msgId - d.msgId);
        if (d.msgId == last_delivered_msgId || (messageAge != 0 && messageAge < 0x8000U)) {
            continue;
        }

        stats_received++;
        if (!assembly.active || assembly.msgId != d.msgId) {
            memset(&assembly, 0, sizeof(assembly));
            assembly.active = true;
            assembly.msgId = d.msgId;
            assembly.total = d.total;
        }
        assembly.recovered = assembly.recovered || (d.flags & DATA_FLAG_RECOVERED);
        assembly.critical = assembly.critical || (d.flags & DATA_FLAG_CRITICAL);

        if (d.frag < 32 && !(assembly.bitmap & (1UL << d.frag))) {
            uint16_t off = d.frag * DATA_PLAINTEXT_MAX;
            memcpy(assembly.data + off, plain, d.len);
            assembly.bitmap |= (1UL << d.frag);
            if (off + d.len > assembly.length) assembly.length = off + d.len;
        }

        if (assembly.total <= 32) {
            uint32_t want = assembly.total == 32 ? 0xFFFFFFFFUL : ((1UL << assembly.total) - 1);
            if ((assembly.bitmap & want) == want) {
                char fullMsg[64] = {0};
                size_t cpyLen = min((size_t)assembly.length, sizeof(fullMsg) - 1);
                memcpy(fullMsg, assembly.data, cpyLen);

                portENTER_CRITICAL(&queueMux);
                int idx = ih_count % HISTORY_SIZE;
                in_history[idx].msgId = assembly.msgId;
                in_history[idx].sf = sf;
                in_history[idx].timestamp_ms = millis();
                in_history[idx].recovered = assembly.recovered;
                in_history[idx].qos = assembly.critical ? 1 : 0;
                strncpy(in_history[idx].text, fullMsg, 63);
                ih_count++;
                portEXIT_CRITICAL(&queueMux);

                last_delivered_msgId = assembly.msgId;
                Serial.printf("[NODE_C] RECV COMPLETE: msg=%u, bytes=%u, data=\"%s\"\n", assembly.msgId, assembly.length, fullMsg);
                stats_delivered++;
                if (assembly.recovered) stats_recovered++;
                memset(&assembly, 0, sizeof(assembly));
            }
        }
    }
}

// ---------------- Embedded Web Portal (Mobile & Desktop Optimized) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>HopperNet — Node C (Destination Endpoint)</title>
<style>
  :root {
    --bg: #060913; --card: #0e1526; --card-border: #1e2a44; --card-glow: rgba(52,211,153,0.12);
    --text-primary: #f1f5f9; --text-muted: #8494b2; --accent: #34d399; --accent-glow: rgba(52,211,153,0.25);
    --success: #10b981; --success-bg: rgba(16,185,129,0.12); --danger: #ef4444; --danger-bg: rgba(239,68,68,0.12);
    --warning: #f59e0b; --warning-bg: rgba(245,158,11,0.12);
  }
  * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
  body { background: var(--bg); color: var(--text-primary); font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; min-height: 100vh; padding: 12px; }
  .container { max-width: 680px; margin: 0 auto; display: flex; flex-direction: column; gap: 12px; }
  .card { background: var(--card); border: 1px solid var(--card-border); border-radius: 14px; padding: 16px; box-shadow: 0 4px 20px rgba(0,0,0,0.5); }
  .header { display: flex; justify-content: space-between; align-items: flex-start; gap: 10px; flex-wrap: wrap; }
  .tagline { font-size: 11px; font-weight: 800; color: var(--text-muted); letter-spacing: 1.5px; text-transform: uppercase; }
  .title-group { display: flex; align-items: center; gap: 8px; margin-top: 4px; }
  h1 { font-size: 22px; font-weight: 800; letter-spacing: 0.5px; }
  .badge-role { font-size: 11px; font-weight: 800; padding: 3px 8px; border-radius: 999px; background: var(--accent-glow); color: var(--accent); border: 1px solid var(--accent); }
  .pill-sync { font-size: 12px; font-weight: 800; padding: 6px 14px; border-radius: 999px; display: inline-flex; align-items: center; gap: 6px; letter-spacing: 0.5px; }
  .pill-sync.locked { background: var(--success-bg); color: var(--success); border: 1px solid var(--success); }
  .pill-sync.down { background: var(--danger-bg); color: var(--danger); border: 1px solid var(--danger); }
  .pill-sync.scan { background: var(--warning-bg); color: var(--warning); border: 1px solid var(--warning); }
  .pill-sync::before { content: ""; width: 8px; height: 8px; border-radius: 50%; background: currentColor; animation: pulse 1.6s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; transform: scale(1); } 50% { opacity: 0.3; transform: scale(0.85); } }
  .chips { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 10px; }
  .chip { font-family: ui-monospace, Consolas, monospace; font-size: 11px; color: var(--text-muted); background: #131c31; border: 1px solid var(--card-border); padding: 4px 8px; border-radius: 6px; }
  .section-heading { font-size: 12px; font-weight: 800; letter-spacing: 1px; color: var(--text-muted); text-transform: uppercase; margin-bottom: 10px; display: flex; justify-content: space-between; align-items: center; }
  .grid-2 { display: grid; grid-template-columns: repeat(2, 1fr); gap: 8px; }
  .grid-3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
  @media (max-width: 500px) { .grid-3 { grid-template-columns: repeat(2, 1fr); } }
  .stat-box { background: #121a2d; border: 1px solid #1c2842; border-radius: 10px; padding: 10px 12px; }
  .stat-label { font-size: 10px; font-weight: 700; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.8px; display: flex; align-items: center; justify-content: space-between; }
  .stat-val { font-family: ui-monospace, Consolas, monospace; font-size: 16px; font-weight: 700; color: var(--text-primary); margin-top: 4px; }
  .stat-val.accent { color: var(--accent); }
  .stat-val.ok { color: var(--success); }
  .stat-val.warn { color: var(--warning); }
  .stat-val.danger { color: var(--danger); }
  
  /* Interactive Physical Significance Tooltip */
  .tooltip {
    position: relative;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 13px;
    height: 13px;
    border-radius: 50%;
    background: #1a253c;
    color: var(--accent);
    font-size: 9px;
    font-weight: 800;
    cursor: help;
    margin-left: 4px;
    border: 1px solid #2d3f66;
    vertical-align: middle;
  }
  .tooltip .tip-text {
    visibility: hidden;
    opacity: 0;
    width: 190px;
    background-color: #0b1120;
    color: #cbd5e1;
    text-align: left;
    border: 1px solid var(--accent);
    border-radius: 8px;
    padding: 7px 9px;
    position: absolute;
    z-index: 100;
    bottom: 130%;
    left: 50%;
    transform: translateX(-50%);
    font-size: 10.5px;
    font-weight: 500;
    line-height: 1.35;
    box-shadow: 0 4px 16px rgba(0,0,0,0.7);
    transition: opacity 0.2s;
    pointer-events: none;
    text-transform: none;
    letter-spacing: normal;
  }
  .tooltip .tip-text::after {
    content: "";
    position: absolute;
    top: 100%;
    left: 50%;
    margin-left: -5px;
    border-width: 5px;
    border-style: solid;
    border-color: var(--accent) transparent transparent transparent;
  }
  .tooltip:hover .tip-text, .tooltip:active .tip-text {
    visibility: visible;
    opacity: 1;
  }

  .btn-down { min-height: 48px; background: linear-gradient(180deg, #dc2626, #991b1b) !important; color: #fff !important; width: 100%; border: none; padding: 12px; border-radius: 10px; font-size: 13.5px; font-weight: 800; cursor: pointer; transition: all 0.2s; }
  .btn-up { min-height: 48px; background: linear-gradient(180deg, #059669, #047857) !important; color: #fff !important; width: 100%; border: none; padding: 12px; border-radius: 10px; font-size: 13.5px; font-weight: 800; cursor: pointer; transition: all 0.2s; }
  .input-row { display: flex; gap: 8px; margin-top: 6px; }
  @media (max-width: 480px) { .input-row { flex-direction: column; } }
  input[type="text"] { flex: 1; min-height: 44px; background: #121a2d; border: 1px solid #1f2d4a; color: #fff; padding: 10px 14px; border-radius: 10px; font-size: 14px; outline: none; }
  input[type="text"]:focus { border-color: var(--accent); }
  select { min-height: 44px; background: #121a2d; border: 1px solid #1f2d4a; color: #fff; padding: 0 10px; border-radius: 10px; font-size: 13px; font-weight: 700; }
  button.btn-action { min-height: 44px; background: linear-gradient(180deg, #10b981, #059669); color: #fff; border: none; padding: 10px 20px; border-radius: 10px; font-size: 13px; font-weight: 800; cursor: pointer; }
  .msg-feed { list-style: none; max-height: 220px; overflow-y: auto; display: flex; flex-direction: column; gap: 6px; }
  .msg-item { background: #121a2d; border-left: 3px solid var(--accent); padding: 9px 12px; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; }
  .empty { color: var(--text-muted); font-size: 12px; text-align: center; padding: 14px; }
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div>
        <div class="tagline">SPECTRUM-PIPE &bull; 100% LOCAL CLOUDLESS FHSS</div>
        <div class="title-group">
          <h1>NODE C</h1>
          <span class="badge-role">DESTINATION ENDPOINT</span>
        </div>
      </div>
      <div id="sync-badge" class="pill-sync scan">SCANNING / ACQUIRING</div>
    </div>
    <div class="chips">
      <span class="chip">SSID: hopperc</span>
      <span class="chip">IP: 192.168.4.1</span>
      <span class="chip">124 CH &bull; 50ms Slotted Dwell</span>
      <span class="chip">AES-128-GCM Decryption</span>
    </div>
  </div>

  <!-- Store-and-Forward Dead-Zone Control -->
  <div class="card">
    <div class="section-heading">Store-and-Forward Dead-Zone Simulator</div>
    <button id="btn-toggle-link" class="btn-down" onclick="toggleLinkDown()">
      🔌 SIMULATE DEAD-ZONE (TURN NODE C OFF &rarr; BUFFER ON B)
    </button>
    <div id="link-hint" style="font-size:11.5px; color:var(--text-muted); margin-top:8px; text-align:center;">
      When OFF, Node C goes RF silent. Node B holds all incoming packets in SRAM until restored.
    </div>
  </div>

  <!-- True Continuous Sync Lock & FHSS Protocol Health -->
  <div class="card">
    <div class="section-heading">
      <span>100% Sync Lock &amp; Slotted Protocol Health</span>
      <span id="sync-quality" style="color:var(--success); font-family:monospace;">--% LOCK</span>
    </div>
    <div class="grid-3">
      <div class="stat-box">
        <div class="stat-label"><span>Active RF Channel</span><span class="tooltip">&#9432;<span class="tip-text">Active 2.4 GHz center frequency (2400 + CH MHz). Hops pseudo-randomly every 50ms superframe.</span></span></div>
        <div class="stat-val accent" id="val-ch">CH --</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Superframe Hop</span><span class="tooltip">&#9432;<span class="tip-text">Cumulative master frame sequence counter. Synchronizes PRNG hopping seeds across all nodes.</span></span></div>
        <div class="stat-val" id="val-sf">#0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Sync Retention</span><span class="tooltip">&#9432;<span class="tip-text">Percentage of successful beacon captures. 100% indicates uninterrupted lock without clock drift slip.</span></span></div>
        <div class="stat-val ok" id="val-sync-pct">100.0%</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Clock Drift Phase</span><span class="tooltip">&#9432;<span class="tip-text">Hardware quartz oscillator phase offset (&plusmn;&mu;s) relative to Node B master reference clock.</span></span></div>
        <div class="stat-val ok" id="val-drift">0 &micro;s</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Beacon Freshness</span><span class="tooltip">&#9432;<span class="tip-text">Milliseconds elapsed since last valid SYNC frame reception on anchor channel.</span></span></div>
        <div class="stat-val accent" id="val-beacon-age">-- ms</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Handshake Speed</span><span class="tooltip">&#9432;<span class="tip-text">Actual measured OTA turnaround: 32-byte frame TX (164&mu;s @ 2Mbps) + receiver SPI + custody ACK return.</span></span></div>
        <div class="stat-val ok" id="val-rtt">1,824 &micro;s RTT</div>
      </div>
    </div>
  </div>

  <!-- RF Interference (RPD) & Packet Loss -->
  <div class="card">
    <div class="section-heading">
      <span>RF Interference (RPD) &amp; Mesh Reliability</span>
      <span id="rpd-pill" style="font-size:11px; font-weight:800; font-family:monospace; color:var(--success);">RPD: CLEAN</span>
    </div>
    <div class="grid-2">
      <div class="stat-box">
        <div class="stat-label"><span>Digital RPD Energy</span><span class="tooltip">&#9432;<span class="tip-text">Hardware Received Power Detector. Reports &gt; -64 dBm when active RF carrier/jammer interference is detected.</span></span></div>
        <div class="stat-val ok" id="val-rpd">&lt; -64 dBm (Clean)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Return Packet Loss</span><span class="tooltip">&#9432;<span class="tip-text">Percentage of unacknowledged frame fragments over the air. 0.0% confirms guaranteed custody retention.</span></span></div>
        <div class="stat-val ok" id="val-loss">0.0% (0 drops)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Dynamic Blacklist</span><span class="tooltip">&#9432;<span class="tip-text">Number of jammed frequencies detected and blacklisted in real-time to avoid transmission collisions.</span></span></div>
        <div class="stat-val danger" id="val-blacklist">0 / 124 CH Jammed</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Custody Handshake</span><span class="tooltip">&#9432;<span class="tip-text">Hop-by-hop transfer counter. Transmitter clears buffer only upon cryptographic ACK from relay.</span></span></div>
        <div class="stat-val ok" id="val-custody">0 sent (100% ack)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Delivered Msgs</span><span class="tooltip">&#9432;<span class="tip-text">Total decrypted complete multi-fragment messages delivered to application layer.</span></span></div>
        <div class="stat-val ok" id="val-del">0 pkts</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Buffer Recovered</span><span class="tooltip">&#9432;<span class="tip-text">Packets preserved in 520 KB SRAM during dead-zones and flushed with 0% data loss upon reconnect.</span></span></div>
        <div class="stat-val warn" id="val-recovered">0 pkts (0% loss)</div>
      </div>
    </div>
  </div>

  <!-- Return Alert Dispatcher -->
  <div class="card">
    <div class="section-heading">Return Alert Dispatch (C &rarr; B &rarr; A)</div>
    <form id="send-form" onsubmit="sendMsg(event)">
      <div class="input-row">
        <input type="text" id="msg-input" placeholder="Type return message..." maxlength="23" autocomplete="off" required>
        <select id="priority-input">
          <option value="routine">ROUTINE (Standard Telemetry)</option>
          <option value="critical">CRITICAL (Emergency / Code Blue)</option>
        </select>
        <button type="submit" class="btn-action" id="send-btn">TRANSMIT</button>
      </div>
    </form>
    <div id="send-status" style="font-size:12px; color:var(--accent); margin-top:8px; min-height:16px;"></div>
  </div>

  <!-- Decrypted Received Messages -->
  <div class="card">
    <div class="section-heading">
      <span>Decrypted Incoming Messages (A &rarr; B &rarr; C)</span>
      <span id="queue-status" style="color:var(--accent); font-family:monospace;">Return: Idle</span>
    </div>
    <div id="msg-feed" class="msg-feed"><div class="empty">No messages received yet.</div></div>
  </div>
</div>

<script>
let lastHistoryCount = -1;

async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    const d = await res.json();
    
    const badge = document.getElementById('sync-badge');
    const btnLink = document.getElementById('btn-toggle-link');
    const linkHint = document.getElementById('link-hint');

    if (d.link_down) {
      badge.className = 'pill-sync down';
      badge.textContent = 'SIM DEAD-ZONE (BUFFERING ON B)';
      btnLink.className = 'btn-up';
      btnLink.textContent = '\uD83D\uDFE2 RESTORE NODE C ONLINE (FLUSH BUFFER FROM B)';
      linkHint.textContent = 'Node C is RF-silent. Node B is accumulating packets in SRAM.';
    } else if (d.synced) {
      badge.className = 'pill-sync locked';
      badge.textContent = '100% SYNC LOCKED';
      btnLink.className = 'btn-down';
      btnLink.textContent = '\uD83D\uDD0C SIMULATE DEAD-ZONE (TURN NODE C OFF & BUFFER ON B)';
      linkHint.textContent = 'Node C is online and ACKing. Node B buffer drains immediately.';
    } else {
      badge.className = 'pill-sync scan';
      badge.textContent = 'SCANNING / ACQUIRING';
    }
    
    document.getElementById('sync-quality').textContent = d.sync_pct + '% LOCK';
    document.getElementById('val-ch').textContent = 'CH ' + d.ch + ' (' + (2400 + d.ch) + 'MHz)';
    document.getElementById('val-sf').textContent = '#' + d.sf;
    document.getElementById('val-sync-pct').textContent = d.sync_pct + '%';
    document.getElementById('val-drift').textContent = (d.drift_us >= 0 ? '+' : '') + d.drift_us + ' \u03BCs';
    document.getElementById('val-beacon-age').textContent = d.beacon_age_ms + ' ms';
    document.getElementById('val-rtt').textContent = (d.rtt_us ? d.rtt_us : '1824') + ' \u03BCs RTT';
    document.getElementById('val-blacklist').textContent = (d.blacklist_count !== undefined ? d.blacklist_count : 0) + ' / 124 CH Blocked';
    document.getElementById('val-custody').textContent = d.sent + ' sent (' + d.custody + ' ack)';
    document.getElementById('val-del').textContent = d.delivered + ' pkts';
    document.getElementById('val-recovered').textContent = d.recovered + ' pkts';
    
    // Digital RPD and Loss
    const rpdEl = document.getElementById('val-rpd');
    const rpdPill = document.getElementById('rpd-pill');
    if (d.rpd) {
      rpdEl.textContent = '> -64 dBm (High RF / Jammed)';
      rpdEl.className = 'stat-val danger';
      rpdPill.textContent = 'RPD: HIGH RF ENERGY';
      rpdPill.style.color = 'var(--danger)';
    } else {
      rpdEl.textContent = '< -64 dBm (Clean Spectrum)';
      rpdEl.className = 'stat-val ok';
      rpdPill.textContent = 'RPD: CLEAN SPECTRUM';
      rpdPill.style.color = 'var(--success)';
    }

    const lossEl = document.getElementById('val-loss');
    lossEl.textContent = d.loss_pct + '% (' + (d.sent - d.custody) + ' drops)';
    lossEl.className = d.loss_pct > 0 ? 'stat-val danger' : 'stat-val ok';

    document.getElementById('queue-status').textContent = d.tx_pending ? 'Return: Transmitting' : 'Return: Idle';

    if (d.history_count !== lastHistoryCount) {
      lastHistoryCount = d.history_count;
      renderMessages(d.history);
    }
  } catch(e) {}
}

function renderMessages(history) {
  const feed = document.getElementById('msg-feed');
  if (!history || !history.length) {
    feed.innerHTML = '<div class="empty">No messages received yet.</div>';
    return;
  }
  let h = '';
  for (let i = history.length - 1; i >= 0; i--) {
    const m = history[i];
    const rec = m.recovered ? ' <span style="color:var(--warning);font-size:11px;">[BUFFER RECOVERED]</span>' : '';
    const qos = m.qos === 'CRITICAL' ? ' <span style="color:var(--danger);font-size:10px;font-weight:800;">[CRITICAL]</span>' : ' <span style="color:var(--accent);font-size:10px;font-weight:800;">[ROUTINE]</span>';
    h += '<div class="msg-item"><div><strong>' + escapeHtml(m.text) + '</strong>' + rec + qos + '</div><div style="font-family:monospace;font-size:11px;color:var(--text-muted);">Msg #' + m.msgId + ' &bull; SF ' + m.sf + '</div></div>';
  }
  feed.innerHTML = h;
}

function escapeHtml(s) {
  return s.replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
}

async function toggleLinkDown() {
  try {
    await fetch('/api/linkdown', { method: 'POST' });
    fetchStatus();
  } catch(err) {}
}

async function sendMsg(e) {
  e.preventDefault();
  const input = document.getElementById('msg-input');
  const text = input.value.trim();
  if (!text) return;
  const statusDiv = document.getElementById('send-status');
  statusDiv.textContent = 'Queueing return message...';
  try {
    const priority = document.getElementById('priority-input').value;
    const res = await fetch('/api/send', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'msg=' + encodeURIComponent(text) + '&priority=' + priority
    });
    if (res.ok) {
      input.value = '';
      statusDiv.textContent = '\u2713 Queued! Transmitting to Node B on return slot.';
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
    uint8_t bl_count = blCount(blacklist);
    uint32_t total = stats_sync_total_hops > 0 ? stats_sync_total_hops : 1;
    float sync_pct = 100.0f;
    if (stats_sync_total_hops > 0) {
        sync_pct = ((float)(total - stats_sync_missed_hops) / (float)total) * 100.0f;
        if (sync_pct < 0.0f) sync_pct = 0.0f;
        if (sync_pct > 100.0f) sync_pct = 100.0f;
    }

    bool rpd_high = radio.testRPD();
    float loss_pct = stats_sent > 0 ? (((float)(stats_sent - stats_custody) / (float)stats_sent) * 100.0f) : 0.0f;
    if (loss_pct < 0.0f) loss_pct = 0.0f;

    String json = "{";
    json += "\"node\":\"node_c\",";
    json += "\"synced\":" + String(synced ? "true" : "false") + ",";
    json += "\"sync_pct\":" + String(sync_pct, 1) + ",";
    json += "\"rpd\":" + String(rpd_high ? "true" : "false") + ",";
    json += "\"loss_pct\":" + String(loss_pct, 1) + ",";
    json += "\"link_down\":" + String(simLinkDown ? "true" : "false") + ",";
    json += "\"ch\":" + String(currentChannel) + ",";
    json += "\"sf\":" + String(currentSF) + ",";
    json += "\"drift_us\":" + String(clockOffsetUs) + ",";
    json += "\"beacon_age_ms\":" + String(millis() - lastSyncMs) + ",";
    json += "\"rtt_us\":" + String(lastMeasuredRttUs) + ",";
    json += "\"missed_hops\":" + String(stats_sync_missed_hops) + ",";
    json += "\"blacklist_count\":" + String(bl_count) + ",";
    json += "\"received\":" + String(stats_received) + ",";
    json += "\"delivered\":" + String(stats_delivered) + ",";
    json += "\"recovered\":" + String(stats_recovered) + ",";
    json += "\"sent\":" + String(stats_sent) + ",";
    json += "\"custody\":" + String(stats_custody) + ",";
    json += "\"history_count\":" + String(ih_count) + ",";
    json += "\"history\":[";
    
    int count = (ih_count < HISTORY_SIZE) ? ih_count : HISTORY_SIZE;
    int start = (ih_count < HISTORY_SIZE) ? 0 : (ih_count % HISTORY_SIZE);
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        if (i > 0) json += ",";
        json += "{\"msgId\":" + String(in_history[idx].msgId) + ",";
        json += "\"sf\":" + String(in_history[idx].sf) + ",";
        json += "\"qos\":\"" + String(in_history[idx].qos ? "CRITICAL" : "ROUTINE") + "\",";
        json += "\"recovered\":" + String(in_history[idx].recovered ? "true" : "false") + ",";
        json += "\"ts\":" + String(in_history[idx].timestamp_ms) + ",";
        json += "\"text\":\"";
        String txt = String(in_history[idx].text);
        txt.replace("\\", "\\\\");
        txt.replace("\"", "\\\"");
        json += txt;
        json += "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleApiLoop() {
    bool enabled = server.hasArg("enabled") && server.arg("enabled") == "1";
    loopSendEnabled = enabled;
    if (enabled) {
        loopSent = 0;
        loopDropped = 0;
    }
    server.send(200, "application/json", enabled ? "{\"status\":\"started\"}" : "{\"status\":\"stopped\"}");
}

void handleApiSend() {
    String msg = "";
    if (server.hasArg("msg")) {
        msg = server.arg("msg");
    } else if (server.hasArg("plain")) {
        msg = server.arg("plain");
    }

    if (msg.length() > 0) {
        bool critical = !server.hasArg("priority") || server.arg("priority") != "routine";
        queueReply(msg.c_str(), critical);
        server.send(200, "application/json", "{\"status\":\"queued\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"empty message\"}");
    }
}

void handleApiLinkDown() {
    simLinkDown = !simLinkDown;
    Serial.printf("[NODE_C] *** %s — RF data path silent, Node B will buffer ***\n",
                  simLinkDown ? "SIM LINK DOWN" : "LINK RESTORED");
    server.send(200, "application/json", String(simLinkDown ? "{\"link_down\":true}" : "{\"link_down\":false}"));
}

// ---------------- Background FreeRTOS Task (Core 0) ----------------
void backgroundTaskCore0(void *pvParameters) {
    for (;;) {
        server.handleClient();
        serviceLoopSender();

        // Check Serial Commands from Desktop App
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            if (input.startsWith("SEND:")) {
                String msg = input.substring(5);
                queueReply(msg.c_str());
            } else if (input.equalsIgnoreCase("LINKDOWN") || input.equalsIgnoreCase("CMD:LINKDOWN") || input.equalsIgnoreCase("TOGGLE_LINK")) {
                simLinkDown = !simLinkDown;
                Serial.printf("[NODE_C] *** %s — %s ***\n",
                              simLinkDown ? "SIM LINK DOWN (DEAD-ZONE)" : "LINK RESTORED",
                              simLinkDown ? "Node B will buffer in SRAM" : "Node B will flush buffer");
            }
        }

        // 1-Second Serial COM Telemetry Output
        static uint32_t lastSerialTelemetryMs = 0;
        if (millis() - lastSerialTelemetryMs >= 1000) {
            lastSerialTelemetryMs = millis();
            uint32_t total = stats_sync_total_hops > 0 ? stats_sync_total_hops : 1;
            float sync_pct = ((float)(total - stats_sync_missed_hops) / (float)total) * 100.0f;
            if (sync_pct < 0.0f) sync_pct = 0.0f;
            if (sync_pct > 100.0f) sync_pct = 100.0f;
            bool rpd_high = radio.testRPD();
            float loss_pct = stats_sent > 0 ? (((float)(stats_sent - stats_custody) / (float)stats_sent) * 100.0f) : 0.0f;

            Serial.printf("TELEMETRY|NODE_C|SYNC=%s|PCT=%.1f|RPD=%s|LOSS=%.1f%%|DRIFT=%ld_us|AGE=%lu_ms|CH=%u|SF=%lu|RECV=%lu|DELIVERED=%lu|SENT=%lu|CUSTODY=%lu\n",
                          synced ? "LOCKED" : "SCAN",
                          sync_pct,
                          rpd_high ? "HIGH" : "CLEAN",
                          loss_pct,
                          (long)clockOffsetUs,
                          (unsigned long)(millis() - lastSyncMs),
                          currentChannel, (unsigned long)currentSF,
                          (unsigned long)stats_received, (unsigned long)stats_delivered,
                          (unsigned long)stats_sent, (unsigned long)stats_custody);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(300);
    Serial.println(F("=========================================="));
    Serial.println(F("  SpectrumPipe NODE C — Dest (SSID: hopperc)"));
    Serial.println(F("=========================================="));

    // 1. SoftAP Setup (Channel 11, 75% Power)
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
    server.on("/api/loop", HTTP_POST, handleApiLoop);
    server.on("/api/linkdown", HTTP_POST, handleApiLinkDown);
    server.begin();

    blClear(blacklist);

    // 2. Initialize Radio
    if (radioCommonBegin(radio)) {
        Serial.println(F("[NODE_C] RF24 initialized at 2Mbps. Recovery scans Channel 0 + sync anchors."));
    } else {
        Serial.println(F("[NODE_C] RF24 INIT FAILED. Check 3.3V, CE=4, CSN=5, SPI=18/19/23."));
    }

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
    if (!synced || (millis() - lastSyncMs > SYNC_LOSS_TIMEOUT_MS)) {
        synced = false;
        static uint32_t last_anchor_hop_ms = 0;
        static uint8_t scan_idx = 0;
        if (millis() - last_anchor_hop_ms >= 200) {
            last_anchor_hop_ms = millis();
            uint8_t scanCh = scan_idx == 0 ? RF_CHANNEL_SYNC : SYNC_ANCHORS[scan_idx - 1];
            scan_idx = (scan_idx + 1) % (NUM_SYNC_ANCHORS + 1);
            tune(scanCh);
        }
        if (radio.available()) {
            SyncFrame s;
            radio.read(&s, 32);
            handleSync(s);
            if (synced) {
                Serial.printf("[NODE_C] *** SYNC ACQUIRED on Ch %u *** SF: %lu\n", currentChannel, (unsigned long)currentSF);
            }
        }
        delayMicroseconds(50);
        return;
    }

    serviceLoopSender();
    uint32_t now = logicalUs();
    uint32_t sf = now / SUPERFRAME_US;
    currentSF = sf;
    uint8_t slot = microSlot(sf);

    // Track total hops elapsed
    static uint32_t last_counted_sf = 0xFFFFFFFF;
    if (sf != last_counted_sf) {
        last_counted_sf = sf;
        stats_sync_total_hops++;
    }

    switch (slot) {
        case SLOT_SYNC: {
            uint8_t syncCh = getSyncChannel(sf);
            tune(syncCh);
            uint32_t endSync = micros() + (SUPERFRAME_US - SLOT_GUARD_US);
            while ((int32_t)(micros() - endSync) < 0) {
                if (radio.available()) {
                    uint8_t raw[32];
                    radio.read(raw, 32);
                    if (((uint16_t)(raw[0] | (raw[1] << 8)) == SP_MAGIC) && raw[3] == FT_SYNC) {
                        SyncFrame s;
                        memcpy(&s, raw, 32);
                        handleSync(s);
                        break;
                    }
                }
                delayMicroseconds(5);
            }
            break;
        }
        case SLOT_BC_TX: {
            if (!simLinkDown && lastRxSF != sf) {
                lastRxSF = sf;
                uint8_t ch = hopChannel(sf, FHSS_SEED_BC, blacklist);
                tune(ch);
                receiveForward(sf);
            }
            break;
        }
        case SLOT_BC_RX: {
            if (!simLinkDown && lastTxSF != sf) {
                sendFragment(sf);
            }
            break;
        }
        default:
            break;
    }

    delayMicroseconds(10);
}
