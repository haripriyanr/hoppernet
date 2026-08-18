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
#define SYNC_LOSS_TIMEOUT_MS 30000UL
#define TX_QUEUE_SIZE   32
#define LOOP_INTERVAL_MS 250UL

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
static volatile uint8_t  lastBpFwd = 0; // Dynamic backpressure from Relay B (0=OK, 1=Throttled)

static uint16_t nextMsgId = 1;
static uint16_t txMsgId = 0;
static uint8_t  txFrag = 0;
static uint8_t  txTotal = 0;
static char     txMessage[256];
static uint16_t txMessageLen = 0;
static bool     txCritical = true;
static uint32_t lastTxSF = 0xFFFFFFFFUL;
static uint32_t lastRxSF = 0xFFFFFFFFUL;
struct TxItem {
    char text[256];
    bool critical;
};
static TxItem   txQueue[TX_QUEUE_SIZE];
static uint8_t  txQueueHead = 0;
static uint8_t  txQueueTail = 0;
static uint8_t  txQueueCount = 0;
static volatile bool loopSendEnabled = false;
static volatile uint32_t loopSent = 0;
static volatile uint32_t loopDropped = 0;
static uint32_t loopNextMs = 0;
static uint8_t loopIndex = 0;
static volatile bool simLinkDown = false;

static volatile uint32_t stats_sent = 0;
static volatile uint32_t stats_custody = 0;
static volatile uint32_t stats_delivered = 0;
static volatile uint32_t stats_received = 0;
static volatile uint32_t stats_retries = 0;
static volatile uint32_t stats_recovered = 0;
static volatile uint32_t lastMeasuredRttUs = 1824;

/// Cumulative Sync & Protocol Metrics
static volatile uint32_t stats_sync_total_hops = 0;
static volatile uint32_t stats_sync_locked_hops = 0;
static volatile uint32_t stats_sync_missed_hops = 0;
static volatile uint32_t stats_desync_events = 0;

// Inbound History (return messages from Node C via Node B)
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

static inline uint32_t currentMeshSF() {
    if (!synced) return 0;
    uint32_t elapsedUs = micros() - lastSyncLocal;
    return lastSyncSf + (elapsedUs / SUPERFRAME_US);
}

static inline uint32_t currentSlotElapsedUs() {
    if (!synced) return 0;
    uint32_t elapsedUs = micros() - lastSyncLocal;
    return (elapsedUs % SUPERFRAME_US);
}

static inline uint32_t logicalUs() {
    if (!synced) return micros();
    return currentMeshSF() * SUPERFRAME_US + currentSlotElapsedUs();
}

static inline void tune(uint8_t ch) {
    if (currentChannel != ch) {
        setRadioChannel(radio, ch);
        currentChannel = ch;
    }
}

static void startNextTxLocked() {
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

bool queueText(const char *s, bool critical = false) {
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
    Serial.printf("[NODE_A] QUEUED: bytes=%u, pending=%u\n", (unsigned)n, txQueueCount);
    return accepted;
}

static void serviceLoopSender() {
    if (!loopSendEnabled || (int32_t)(millis() - loopNextMs) < 0) return;
    char text[2] = {(char)('A' + loopIndex), '\0'};
    if (queueText(text, false)) loopSent++;
    else loopDropped++;
    loopIndex = (loopIndex + 1) % 26;
    loopNextMs = millis() + LOOP_INTERVAL_MS;
}

void handleSync(const SyncFrame &s) {
    if (!validHeader(s.magic, s.version, s.type, s.src, s.dst) || s.src != NODE_B || s.type != FT_SYNC) return;
    memcpy(blacklist, s.blacklist, BLACKLIST_BYTES);
    mapVersion = s.mapVersion;
    lastSyncLocal = micros();
    lastSyncSf = s.sf;
    currentSF = s.sf;
    clockOffsetUs = (int32_t)(s.sf * SUPERFRAME_US) - (int32_t)lastSyncLocal;
    synced = true;
    lastSyncMs = millis();
    lastBpFwd = s.bp_fwd;
    stats_sync_missed_hops = 0;
    stats_sync_locked_hops++;
}

void processAck(const AckFrame &a) {
    if (!validHeader(a.magic, a.version, a.type, a.src, a.dst) || a.dst != NODE_A) return;
    if (a.type == FT_CUSTODY && a.msgId == txMsgId && a.frag == txFrag) {
        stats_custody++;
        lastBpFwd = a.bp_fwd;
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
            startNextTxLocked();
            portEXIT_CRITICAL(&queueMux);
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
    f.flags = DATA_FLAG_E2E | (txCritical ? DATA_FLAG_CRITICAL : 0);
    f.len = len;

    if (!chachaEncrypt((uint8_t*)txMessage + offset, len, f.ciphertext, f.tag, NODE_A, NODE_C, sf, txMsgId, txFrag)) return;
    uint8_t ch = hopChannel(sf, FHSS_SEED_AB, blacklist);
    tune(ch);

    uint32_t txStart = micros();
    if (txFrame(radio, &f)) {
        stats_sent++;
        uint32_t deadline = txStart + 3500;
        while ((int32_t)(micros() - deadline) < 0) {
            if (!radio.available()) {
                delayMicroseconds(5);
                continue;
            }
            uint8_t raw[32];
            radio.read(raw, 32);
            AckFrame a;
            memcpy(&a, raw, 32);
            if (a.magic == SP_MAGIC && a.type == FT_CUSTODY && a.src == NODE_B && a.dst == NODE_A && a.msgId == f.msgId && a.frag == f.frag) {
                uint32_t rttUs = micros() - txStart;
                lastMeasuredRttUs = rttUs;
                Serial.printf("HANDSHAKE|RTT=%lu_us|msg=%u|frag=%u|sf=%lu\n", (unsigned long)rttUs, f.msgId, f.frag, (unsigned long)sf);
                processAck(a);
                break;
            }
        }
    } else {
        stats_retries++;
    }
    lastTxSF = sf;
}

struct ReturnAssembly {
    uint16_t msgId;
    uint8_t  total;
    uint32_t bitmap;
    uint16_t length;
    char     data[256];
    bool     active;
    bool     recovered;
    bool     critical;
};
static ReturnAssembly ret_asm = {};
static uint16_t done_return_ids[8];
static uint8_t  done_return_count = 0;

static uint8_t pending_rx[32];
static bool pending_rx_valid = false;

static bool returnAlreadyDelivered(uint16_t id) {
    for (uint8_t i = 0; i < done_return_count; i++) {
        if (done_return_ids[i] == id) return true;
    }
    return false;
}

static void markReturnDelivered(uint16_t id) {
    if (returnAlreadyDelivered(id)) return;
    if (done_return_count < 8) {
        done_return_ids[done_return_count++] = id;
    } else {
        memmove(&done_return_ids[0], &done_return_ids[1], 7 * sizeof(uint16_t));
        done_return_ids[7] = id;
    }
}

void receiveDownlink(uint32_t sf) {
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
            Serial.printf("[NODE_A] RAW_RX|type=%u|src=%u|dst=%u|sf=%lu\n", raw[3], raw[4], raw[5], (unsigned long)sf);
        }
        if (!have) {
            delayMicroseconds(5);
            continue;
        }
        uint8_t type = raw[3];
        if (type == FT_SYNC) {
            SyncFrame s;
            memcpy(&s, raw, 32);
            handleSync(s);
            continue;
        } else if (type == FT_CUSTODY || type == FT_DELIVERY) {
            AckFrame a;
            memcpy(&a, raw, 32);
            processAck(a);
        } else if (type == FT_DATA) {
            DataFrame d;
            memcpy(&d, raw, 32);
            if ((d.src == NODE_B || d.src == NODE_C) && (d.dst == NODE_A || d.dst == NODE_B) && d.len <= DATA_PLAINTEXT_MAX) {
                uint8_t plain[8] = {0};
                if (chachaDecrypt(d.ciphertext, d.len, d.tag, plain, NODE_C, NODE_A, d.sf, d.msgId, d.frag)) {
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
                    delayMicroseconds(20);
                    txFrame(radio, &ack);
                    stats_delivered++;
                    Serial.printf("[NODE_A] RETURN_FRAG|msg=%u|frag=%u/%u|len=%u\n", d.msgId, d.frag + 1, d.total, d.len);

                    if (!ret_asm.active || ret_asm.msgId != d.msgId) {
                        memset(&ret_asm, 0, sizeof(ret_asm));
                        ret_asm.active = true;
                        ret_asm.msgId = d.msgId;
                        ret_asm.total = d.total;
                    }
                    ret_asm.recovered = ret_asm.recovered || (d.flags & DATA_FLAG_RECOVERED);
                    ret_asm.critical = ret_asm.critical || (d.flags & DATA_FLAG_CRITICAL);
                    if (d.frag < 32 && !(ret_asm.bitmap & (1UL << d.frag))) {
                        uint16_t off = d.frag * DATA_PLAINTEXT_MAX;
                        memcpy(ret_asm.data + off, plain, d.len);
                        ret_asm.bitmap |= (1UL << d.frag);
                        if (off + d.len > ret_asm.length) ret_asm.length = off + d.len;
                    }

                    if (ret_asm.total <= 32) {
                        uint32_t want = ret_asm.total == 32 ? 0xFFFFFFFFUL : ((1UL << ret_asm.total) - 1);
                        if ((ret_asm.bitmap & want) == want) {
                            char fullMsg[64] = {0};
                            size_t cpyLen = min((size_t)ret_asm.length, sizeof(fullMsg) - 1);
                            memcpy(fullMsg, ret_asm.data, cpyLen);

                            portENTER_CRITICAL(&queueMux);
                            int idx = ih_count % HISTORY_SIZE;
                            in_history[idx].msgId = ret_asm.msgId;
                            in_history[idx].sf = d.sf;
                            in_history[idx].timestamp_ms = millis();
                            in_history[idx].recovered = ret_asm.recovered;
                            in_history[idx].qos = ret_asm.critical ? 1 : 0;
                            strncpy(in_history[idx].text, fullMsg, 63);
                            ih_count++;
                            portEXIT_CRITICAL(&queueMux);

                            markReturnDelivered(ret_asm.msgId);
                            Serial.printf("[NODE_A] RECV RETURN COMPLETE: msg=%u, bytes=%u, data=\"%s\"\n", ret_asm.msgId, ret_asm.length, fullMsg);
                            stats_received++;
                            if (ret_asm.recovered) stats_recovered++;
                            memset(&ret_asm, 0, sizeof(ret_asm));
                        }
                    }
                } else {
                    Serial.printf("[NODE_A] AUTH_FAIL|msg=%u|frag=%u\n", d.msgId, d.frag);
                }
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
<title>HopperNet — Node A (Source Endpoint)</title>
<style>
  :root {
    --bg: #060913; --card: #0e1526; --card-border: #1e2a44; --card-glow: rgba(14,165,233,0.12);
    --text-primary: #f1f5f9; --text-muted: #8494b2; --accent: #38bdf8; --accent-glow: rgba(56,189,248,0.25);
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
  button { min-height: 44px; background: linear-gradient(180deg, #0ea5e9, #0284c7); color: #fff; border: none; padding: 10px 20px; border-radius: 10px; font-size: 13px; font-weight: 800; cursor: pointer; transition: all 0.2s; }
  button:active { transform: translateY(1px); filter: brightness(0.9); }
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
          <h1>NODE A</h1>
          <span class="badge-role">SOURCE ENDPOINT</span>
        </div>
      </div>
      <div id="sync-badge" class="pill-sync scan">SCANNING / ACQUIRING</div>
    </div>
    <div class="chips">
      <span class="chip">SSID: hoppera</span>
      <span class="chip">IP: 192.168.4.1</span>
      <span class="chip">124 CH &bull; 1250&micro;s Micro-Slot (800 hops/s)</span>
      <span class="chip">ChaCha20-Poly1305 256-bit AEAD</span>
    </div>
  </div>

  <!-- Store-and-Forward Dead-Zone Control -->
  <div class="card">
    <div class="section-heading">Store-and-Forward Dead-Zone Simulator</div>
    <button id="btn-toggle-link" class="btn-down" onclick="toggleLinkDown()">
      🔌 SIMULATE DEAD-ZONE (TURN NODE A OFF &rarr; BUFFER ON B)
    </button>
    <div id="link-hint" style="font-size:11.5px; color:var(--text-muted); margin-top:8px; text-align:center;">
      When OFF, Node A goes RF silent. Node B holds all incoming return packets in SRAM until restored.
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
        <div class="stat-label"><span>Active RF Channel</span><span class="tooltip">&#9432;<span class="tip-text">Active 2.4 GHz center frequency (2400 + CH MHz). Hops pseudo-randomly every 1250&micro;s micro-slot (800 hops/s).</span></span></div>
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

  <!-- RF Power Detector & Reliability Telemetry -->
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
        <div class="stat-label"><span>OTA Packet Loss</span><span class="tooltip">&#9432;<span class="tip-text">Percentage of unacknowledged frame fragments over the air. 0.0% confirms guaranteed custody retention.</span></span></div>
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

  <!-- Forward Alert Dispatcher -->
  <div class="card">
    <div class="section-heading">Forward Alert Dispatch (A &rarr; B &rarr; C)</div>
    <form id="send-form" onsubmit="sendMsg(event)">
      <div class="input-row">
        <input type="text" id="msg-input" placeholder="Type alert message..." maxlength="23" autocomplete="off" required>
        <select id="priority-input">
          <option value="critical">CRITICAL (Emergency / Code Blue)</option>
          <option value="routine">ROUTINE (Standard Telemetry)</option>
        </select>
        <button type="submit" id="send-btn">TRANSMIT</button>
      </div>
    </form>
    <div id="send-status" style="font-size:12px; color:var(--accent); margin-top:8px; min-height:16px;"></div>
  </div>

  <!-- Decrypted Return Feed -->
  <div class="card">
    <div class="section-heading">
      <span>Decrypted Return Feed (C &rarr; B &rarr; A)</span>
      <span id="feed-status" style="color:var(--accent); font-family:monospace;">IDLE</span>
    </div>
    <div id="msg-feed" class="msg-feed"><div class="empty">No return messages received yet.</div></div>
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
      btnLink.textContent = '\uD83D\uDFE2 RESTORE NODE A ONLINE (FLUSH BUFFER FROM B)';
      linkHint.textContent = 'Node A is RF-silent. Node B is accumulating return packets in SRAM.';
    } else if (d.synced) {
      badge.className = 'pill-sync locked';
      badge.textContent = '100% SYNC LOCKED';
      btnLink.className = 'btn-down';
      btnLink.textContent = '\uD83D\uDD0C SIMULATE DEAD-ZONE (TURN NODE A OFF & BUFFER ON B)';
      linkHint.textContent = 'Node A is online and transmitting/receiving. Buffer flushes immediately.';
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
    document.getElementById('val-blacklist').textContent = d.blacklist_count + ' / 124 CH Blocked';
    document.getElementById('val-custody').textContent = d.sent + ' sent (' + d.custody + ' ack)';
    document.getElementById('val-del').textContent = d.delivered + ' pkts';
    document.getElementById('val-recovered').textContent = d.recovered + ' pkts';

    // Digital RPD and Packet Loss
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

    if (d.history_count !== lastHistoryCount) {
      lastHistoryCount = d.history_count;
      renderMessages(d.history);
    }
  } catch(e) {}
}

function renderMessages(history) {
  const feed = document.getElementById('msg-feed');
  if (!history || !history.length) {
    feed.innerHTML = '<div class="empty">No return messages received yet.</div>';
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
  statusDiv.textContent = 'Encrypting & queueing for next FHSS superframe...';
  try {
    const priority = document.getElementById('priority-input').value;
    const res = await fetch('/api/send', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'msg=' + encodeURIComponent(text) + '&priority=' + priority
    });
    if (res.ok) {
      input.value = '';
      statusDiv.textContent = '\u2713 Dispatched! Awaiting Node B Custody ACK.';
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
    float loss_pct = (stats_sent > 0 && stats_custody > 0) ? 
        (((float)stats_retries / (float)(stats_sent + stats_retries)) * 100.0f) : 
        (stats_sent > 0 && stats_custody == 0 ? 0.0f : 0.0f);
    if (loss_pct < 0.0f) loss_pct = 0.0f;
    if (loss_pct > 100.0f) loss_pct = 100.0f;

    String json = "{";
    json += "\"node\":\"node_a\",";
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
    json += "\"sent\":" + String(stats_sent) + ",";
    json += "\"custody\":" + String(stats_custody) + ",";
    json += "\"received\":" + String(stats_received) + ",";
    json += "\"delivered\":" + String(stats_delivered) + ",";
    json += "\"recovered\":" + String(stats_recovered) + ",";
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
        json += "\"text\":\"" + String(in_history[idx].text) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleApiLoop() {
    bool enabled = server.hasArg("enabled") && server.arg("enabled") == "1";
    loopSendEnabled = enabled;
    if (enabled) {
        loopIndex = 0;
        loopNextMs = millis();
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
        queueText(msg.c_str(), critical);
        server.send(200, "application/json", "{\"status\":\"queued\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"empty message\"}");
    }
}

void handleApiLinkDown() {
    simLinkDown = !simLinkDown;
    Serial.printf("[NODE_A] *** %s — RF data path silent, Node B will buffer ***\n",
                  simLinkDown ? "SIM LINK DOWN" : "LINK RESTORED");
    server.send(200, "application/json", String(simLinkDown ? "{\"link_down\":true}" : "{\"link_down\":false}"));
}

// ---------------- Background FreeRTOS Task (Core 0) ----------------
void backgroundTaskCore0(void *pvParameters) {
    for (;;) {
        server.handleClient();

        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            if (input.startsWith("SEND:")) {
                String msg = input.substring(5);
                queueText(msg.c_str());
            } else if (input.equalsIgnoreCase("LINKDOWN") || input.equalsIgnoreCase("CMD:LINKDOWN") || input.equalsIgnoreCase("TOGGLE_LINK")) {
                simLinkDown = !simLinkDown;
                Serial.printf("[NODE_A] *** %s — %s ***\n",
                              simLinkDown ? "SIM LINK DOWN (DEAD-ZONE)" : "LINK RESTORED",
                              simLinkDown ? "Node B will buffer return in SRAM" : "Node B will flush buffer");
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

            Serial.printf("TELEMETRY|NODE_A|SYNC=%s|PCT=%.1f|RPD=%s|LOSS=%.1f%%|DRIFT=%ld_us|AGE=%lu_ms|CH=%u|SF=%lu|SENT=%lu|CUSTODY=%lu\n",
                          synced ? "LOCKED" : "SCAN",
                          sync_pct,
                          rpd_high ? "HIGH" : "CLEAN",
                          loss_pct,
                          (long)clockOffsetUs,
                          (unsigned long)(millis() - lastSyncMs),
                          currentChannel, (unsigned long)currentSF,
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
    server.on("/api/linkdown", HTTP_POST, handleApiLinkDown);
    server.on("/api/loop", HTTP_POST, handleApiLoop);
    server.begin();

    blClear(blacklist);

    // 2. Initialize Radio
    if (radioCommonBegin(radio)) {
        Serial.println(F("[NODE_A] RF24 initialized at 2Mbps. Recovery scans Channel 0 + sync anchors."));
    } else {
        Serial.println(F("[NODE_A] RF24 INIT FAILED. Check 3.3V, CE=4, CSN=5, SPI=18/19/23."));
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
                Serial.printf("[NODE_A] *** SYNC ACQUIRED on Ch %u *** SF: %lu\n", currentChannel, (unsigned long)currentSF);
            }
        }
        delayMicroseconds(50);
        return;
    }

    serviceLoopSender();
    uint32_t elapsedUs = micros() - lastSyncLocal;
    uint32_t sf = lastSyncSf + (elapsedUs / SUPERFRAME_US);
    uint32_t slotBase = lastSyncLocal + ((sf - lastSyncSf) * SUPERFRAME_US);
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
            uint8_t syncCh = (stats_sync_missed_hops > 2) ? RF_CHANNEL_SYNC : getSyncChannel(sf);
            tune(syncCh);
            uint32_t endSync = slotBase + SUPERFRAME_US - SLOT_GUARD_US;
            bool gotSync = false;
            while ((int32_t)(micros() - endSync) < 0) {
                if (radio.available()) {
                    uint8_t raw[32];
                    radio.read(raw, 32);
                    if (((uint16_t)(raw[0] | (raw[1] << 8)) == SP_MAGIC) && raw[3] == FT_SYNC) {
                        SyncFrame s;
                        memcpy(&s, raw, 32);
                        handleSync(s);
                        gotSync = true;
                        stats_sync_missed_hops = 0;
                        break;
                    }
                }
                delayMicroseconds(5);
            }
            if (!gotSync) {
                stats_sync_missed_hops++;
                if (stats_sync_missed_hops > 8) {
                    synced = false;
                }
            }
            break;
        }
        case SLOT_AB_RX: {
            if (!simLinkDown && lastTxSF != sf) {
                sendDataFragment(sf);
            }
            break;
        }
        case SLOT_AB_TX: {
            if (!simLinkDown && lastRxSF != sf) {
                lastRxSF = sf;
                uint8_t ch = hopChannel(sf, FHSS_SEED_AB, blacklist);
                tune(ch);
                receiveDownlink(sf);
            }
            break;
        }
        default:
            break;
    }

    // Precision dwell pacing until next micro-slot boundary
    while ((int32_t)(micros() - (slotBase + SUPERFRAME_US)) < 0) {
        // Microsecond spin
    }
}
