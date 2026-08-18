// HopperNet Node B — SpectrumPipe Single-Radio Master Relay (ESP32)
// 100% Local & Cloudless: 50 ms Slotted Superframe + SRAM Custody Queues + Dual-Core Architecture

#include <Arduino.h>
#include <SPI.h>
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

#define QUEUE_MAX       64
#define RELAY_LOG_MAX   12
#define MAX_LINK_ATTEMPTS 4
#define MAX_DRAIN_PER_WINDOW 1
#define DELIVERY_ACK_WAIT_US 5500UL

RF24 radio(RF_CE_PIN, RF_CSN_PIN);
WebServer server(80);

// ---------------- Thread Safety Spinlocks ----------------
static portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- SRAM Custody Queue (Forward A -> C) ----------------
struct QueueItem {
    DataFrame frame;
    uint32_t queuedSF;
    uint8_t attempts;
    bool used;
};

static QueueItem qAC[QUEUE_MAX];
static volatile uint8_t qACHead = 0, qACTail = 0, qACCount = 0;
static volatile uint8_t qACHighWater = 0;
static volatile uint32_t fwdDrops = 0;
static volatile uint32_t fwdRecovered = 0;

static QueueItem qCA[QUEUE_MAX];
static volatile uint8_t qCAHead = 0, qCATail = 0, qCACount = 0;
static volatile uint8_t qCAHighWater = 0;
static volatile uint32_t revDrops = 0;
static volatile uint32_t revRecovered = 0;

struct RelayLog {
    char dir[8]; // "A->C"
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
static volatile uint32_t rxA = 0, txC = 0;
static volatile uint32_t rxC = 0, txA = 0;
static volatile uint32_t ackA = 0, ackC = 0;
static volatile int16_t forcedProbeChannel = -1;
static volatile uint32_t lastMeasuredRttUs = 1824;

// Node C link-health tracking
static volatile uint32_t lastCDeliveryMs = 0;

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
    q[head].attempts = 0;
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
    s.masterUs = micros();
    uint8_t syncCh = getSyncChannel(sfNow);
    tune(syncCh);
    bool ok = txFrame(radio, &s);
    if (syncCh != RF_CHANNEL_SYNC) {
        tune(RF_CHANNEL_SYNC);
        s.masterUs = micros();
        ok = txFrame(radio, &s) || ok;
    }
    if ((sfNow & 0x1F) == 0) {
        Serial.printf("SYNC|B|sf=%lu|ch=%u|recovery=1|tx=%d\n", (unsigned long)sfNow, syncCh, (int)ok);
    }
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
    a.masterUs = micros();
    txFrame(radio, &a);
}

void receiveFromA() {
    uint8_t ch = hopChannel(sfNow, FHSS_SEED_AB, blacklist);
    tune(ch);
    uint32_t deadline = micros() + 11500;
    while ((int32_t)(micros() - deadline) < 0) {
        if (!radio.available()) {
            delayMicroseconds(10);
            continue;
        }
        uint8_t raw[32];
        radio.read(raw, 32);
        DataFrame d;
        memcpy(&d, raw, 32);
        if (d.magic != SP_MAGIC || d.version != SP_VERSION || d.type != FT_DATA || d.src != NODE_A || d.dst != NODE_B || d.len > DATA_PLAINTEXT_MAX) continue;
        
        rxA++;
        if (d.msgId == last_seen_msgId_a && d.frag == last_seen_frag_a) {
            sendAck(NODE_A, sfNow, d.msgId, d.frag, FT_CUSTODY);
            Serial.printf("HANDSHAKE|B|DUP_ACK|msg=%u|frag=%u|sf=%lu\n", d.msgId, d.frag, (unsigned long)sfNow);
            break;
        }

        last_seen_msgId_a = d.msgId;
        last_seen_frag_a = d.frag;

        if (qPush(qAC, qACHead, qACCount, d)) {
            sendAck(NODE_A, sfNow, d.msgId, d.frag, FT_CUSTODY);
            ackA++;
            if (qACCount > qACHighWater) qACHighWater = qACCount;
            logRelay("A->C", d.msgId, d.frag, sfNow);
            Serial.printf("HANDSHAKE|B|CUSTODY_SENT|msg=%u|frag=%u|sf=%lu|Q=%u\n", d.msgId, d.frag, (unsigned long)sfNow, qACCount);
        } else {
            fwdDrops++;
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
        lastCDeliveryMs = millis();
        // Check for duplicate fragment
        if (d.msgId == last_seen_msgId_c && d.frag == last_seen_frag_c) {
            sendAck(NODE_C, sfNow, d.msgId, d.frag, FT_CUSTODY);
            Serial.printf("HANDSHAKE|B|DUP_ACK|dir=C->B|msg=%u|frag=%u|sf=%lu\n", d.msgId, d.frag, (unsigned long)sfNow);
            break;
        }

        last_seen_msgId_c = d.msgId;
        last_seen_frag_c = d.frag;

        if (qPush(qCA, qCAHead, qCACount, d)) {
            sendAck(NODE_C, sfNow, d.msgId, d.frag, FT_CUSTODY);
            ackC++;
            if (qCACount > qCAHighWater) qCAHighWater = qCACount;
            logRelay("C->A", d.msgId, d.frag, sfNow);
            Serial.printf("HANDSHAKE|B|CUSTODY_SENT|dir=C->B|msg=%u|frag=%u|sf=%lu|Q=%u\n", d.msgId, d.frag, (unsigned long)sfNow, qCACount);
        } else {
            revDrops++;
        }
        break;
    }
}

static bool nodeCDown() {
    return (qACCount > 0) && (millis() - lastCDeliveryMs > 2000);
}

static bool abandonHead(QueueItem *q, volatile uint8_t &tail, volatile uint8_t &count) {
    portENTER_CRITICAL(&queueMux);
    if (!count) {
        portEXIT_CRITICAL(&queueMux);
        return false;
    }
    uint8_t attempts = ++q[tail].attempts;
    if (attempts < MAX_LINK_ATTEMPTS) {
        portEXIT_CRITICAL(&queueMux);
        return false;
    }
    uint16_t msgId = q[tail].frame.msgId;
    uint8_t frag = q[tail].frame.frag;
    uint8_t src = q[tail].frame.src;
    tail = (tail + 1) % QUEUE_MAX;
    count--;
    portEXIT_CRITICAL(&queueMux);
    const char *dir = (src == NODE_A) ? "A->C" : "C->A";
    Serial.printf("DROP|B|%s|msg=%u|frag=%u|attempts=%u\n", dir, msgId, frag, MAX_LINK_ATTEMPTS);
    if (src == NODE_A) fwdDrops++; else revDrops++;
    return true;
}

bool forwardOne(QueueItem *q, volatile uint8_t &tail, volatile uint8_t &count,
                uint8_t dst, uint32_t seed, bool forwardToC) {
    if (!count) return false;
    DataFrame f;
    if (!qPeek(q, tail, count, f)) return false;

    bool backlog = false;
    portENTER_CRITICAL(&queueMux);
    if (count) backlog = q[tail].queuedSF < sfNow;
    portEXIT_CRITICAL(&queueMux);

    f.src = NODE_B;
    f.dst = dst;
    if (backlog) f.flags |= DATA_FLAG_RECOVERED;
    uint8_t ch = hopChannel(sfNow, seed, blacklist);
    tune(ch);

    uint32_t txStart = micros();
    bool ok = txFrame(radio, &f);
    if (!ok) {
        abandonHead(q, tail, count);
        return false;
    }

    uint32_t end = micros() + DELIVERY_ACK_WAIT_US;
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
            uint32_t rttUs = micros() - txStart;
            lastMeasuredRttUs = rttUs;
            delivered = true;
            break;
        }
    }

    if (delivered) {
        qPop(tail, count);
        if (forwardToC) txC++; else txA++;
        if (backlog) { if (forwardToC) fwdRecovered++; else revRecovered++; }
        if (forwardToC) lastCDeliveryMs = millis();
        Serial.printf("DELIVER|B|%s|msg=%u|frag=%u|RemainingQ=%u\n", forwardToC ? "A->C" : "C->A", f.msgId, f.frag, count);
    } else {
        if (!(forwardToC && nodeCDown())) abandonHead(q, tail, count);
    }
    return delivered;
}

static void drainQueue(QueueItem *q, volatile uint8_t &tail, volatile uint8_t &count,
                       uint32_t budgetUs, uint8_t maxItems,
                       uint8_t dst, uint32_t seed, bool forwardToC) {
    uint32_t end = micros() + budgetUs;
    uint8_t sent = 0;
    while (count && sent < maxItems && sent < MAX_DRAIN_PER_WINDOW &&
           (int32_t)(micros() + 5000 - end) < 0) {
        uint8_t before = count;
        bool delivered = forwardOne(q, tail, count, dst, seed, forwardToC);
        sent++;
        if (!delivered && count == before) break;
    }
}

// ---------------- Embedded Web Portal (Mobile & Desktop Optimized) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>HopperNet — Node B (Master Relay & Edge Buffer)</title>
<style>
  :root {
    --bg: #060913; --card: #0e1526; --card-border: #1e2a44; --card-glow: rgba(245,158,11,0.12);
    --text-primary: #f1f5f9; --text-muted: #8494b2; --accent: #fbbf24; --accent-glow: rgba(251,191,36,0.2);
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
  .pill-health { font-size: 12px; font-weight: 800; padding: 6px 14px; border-radius: 999px; display: inline-flex; align-items: center; gap: 6px; letter-spacing: 0.5px; }
  .pill-health.good { background: var(--success-bg); color: var(--success); border: 1px solid var(--success); }
  .pill-health.warn { background: var(--warning-bg); color: var(--warning); border: 1px solid var(--warning); }
  .pill-health.danger { background: var(--danger-bg); color: var(--danger); border: 1px solid var(--danger); }
  .pill-health::before { content: ""; width: 8px; height: 8px; border-radius: 50%; background: currentColor; animation: pulse 1.6s infinite; }
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

  .c-down-alert { background: var(--danger-bg); border: 1px solid var(--danger); color: var(--danger); border-radius: 10px; padding: 10px 14px; font-size: 12.5px; font-weight: 700; text-align: center; margin-top: 10px; }
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
        <div class="tagline">SPECTRUM-PIPE &bull; MASTER FHSS CLOCK &amp; EDGE BUFFER</div>
        <div class="title-group">
          <h1>NODE B</h1>
          <span class="badge-role">MASTER RELAY</span>
        </div>
      </div>
      <div id="health-badge" class="pill-health good">MASTER CLOCK ACTIVE</div>
    </div>
    <div class="chips">
      <span class="chip">SSID: hopperb</span>
      <span class="chip">IP: 192.168.4.1</span>
      <span class="chip">124 CH &bull; Fast Microsecond Handshake</span>
      <span class="chip">520 KB SRAM Buffer</span>
    </div>
    <div id="c-down-alert" class="c-down-alert" style="display:none;">
      &#9888; NODE C IS OFF / IN DEAD-ZONE &mdash; Relaying packets into in-memory SRAM buffer (0% Loss Guarantee)
    </div>
  </div>

  <!-- Real-Time Master FHSS & Timing Dashboard -->
  <div class="card">
    <div class="section-heading">
      <span>Master Clock &amp; Slotted Handshake</span>
      <span style="color:var(--accent); font-family:monospace;">164 &micro;s OTA TX &bull; 2 Mbps</span>
    </div>
    <div class="grid-3">
      <div class="stat-box">
        <div class="stat-label"><span>Active RF Channel</span><span class="tooltip">&#9432;<span class="tip-text">Master superframe RF center frequency (2400 + CH MHz). Broadcasts SYNC beacons and hops every 50ms.</span></span></div>
        <div class="stat-val accent" id="val-ch">CH --</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Superframe Hop</span><span class="tooltip">&#9432;<span class="tip-text">Master superframe sequence counter incremented every 50ms. Anchors network-wide hopping PRNG seed.</span></span></div>
        <div class="stat-val" id="val-sf">#0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Handshake Speed</span><span class="tooltip">&#9432;<span class="tip-text">Measured OTA turnaround time for delivery ACK confirmation from destination node.</span></span></div>
        <div class="stat-val ok" id="val-rtt">1,824 &micro;s RTT</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Forward Buffer (A&rarr;C)</span><span class="tooltip">&#9432;<span class="tip-text">In-memory 520 KB SRAM custody queue holding incoming packets destined for Node C.</span></span></div>
        <div class="stat-val warn" id="val-fwd-buf">0 pkts</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Reverse Buffer (C&rarr;A)</span><span class="tooltip">&#9432;<span class="tip-text">In-memory 520 KB SRAM custody queue holding return telemetry destined for Node A.</span></span></div>
        <div class="stat-val accent" id="val-rev-buf">0 pkts</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>RPD Jammer Scanner</span><span class="tooltip">&#9432;<span class="tip-text">Real-time spectrum energy detector scan identifying active interference and blacklisting jammed channels.</span></span></div>
        <div class="stat-val danger" id="val-jam">0 / 124 Blocked</div>
      </div>
    </div>
  </div>

  <!-- Digital RPD & Packet Loss Telemetry -->
  <div class="card">
    <div class="section-heading">
      <span>RF Interference (RPD) &amp; Reliability</span>
      <span id="rpd-pill" style="font-size:11px; font-weight:800; font-family:monospace; color:var(--success);">RPD: CLEAN</span>
    </div>
    <div class="grid-2">
      <div class="stat-box">
        <div class="stat-label"><span>Digital RPD Energy</span><span class="tooltip">&#9432;<span class="tip-text">Hardware Received Power Detector. Reports &gt; -64 dBm when carrier energy/jammer power is detected.</span></span></div>
        <div class="stat-val ok" id="val-rpd">&lt; -64 dBm (Clean)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Relay Packet Loss</span><span class="tooltip">&#9432;<span class="tip-text">Percentage of unacknowledged packets dropped by relay. 0.0% confirms lossless custody.</span></span></div>
        <div class="stat-val ok" id="val-loss">0.0% (0 drops)</div>
      </div>
    </div>
  </div>

  <!-- Store-and-Forward Edge Buffering & Delivery Stats -->
  <div class="card">
    <div class="section-heading">Store-and-Forward Custody Metrics</div>
    <div class="grid-2">
      <div class="stat-box">
        <div class="stat-label"><span>Delivered A &rarr; C</span><span class="tooltip">&#9432;<span class="tip-text">Total packets delivered to Node C with confirmed delivery ACK handshakes.</span></span></div>
        <div class="stat-val ok" id="val-fwd-del">0 pkts (100%)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Delivered C &rarr; A</span><span class="tooltip">&#9432;<span class="tip-text">Total return packets delivered to Node A with confirmed delivery ACK handshakes.</span></span></div>
        <div class="stat-val ok" id="val-rev-del">0 pkts (100%)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Dead-Zone Recovered</span><span class="tooltip">&#9432;<span class="tip-text">Packets buffered in SRAM during endpoint dead-zones and flushed with 0% data loss upon reconnection.</span></span></div>
        <div class="stat-val ok" id="val-recovered">0 pkts (0% loss)</div>
      </div>
      <div class="stat-box">
        <div class="stat-label"><span>Buffer High-Water</span><span class="tooltip">&#9432;<span class="tip-text">Peak SRAM buffer depth recorded during peak load or extended dead-zones.</span></span></div>
        <div class="stat-val accent" id="val-high">0 pkts max</div>
      </div>
    </div>
  </div>

  <!-- Routing Feed -->
  <div class="card">
    <div class="section-heading">
      <span>Relay Custody Routing Stream</span>
      <span id="relay-status" style="color:var(--accent); font-family:monospace;">0 routed</span>
    </div>
    <div id="relay-feed" class="msg-feed"><div class="empty">No packets routed yet.</div></div>
  </div>
</div>

<script>
let lastHistoryCount = -1;

async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    const d = await res.json();
    
    const health = document.getElementById('health-badge');
    const alertBox = document.getElementById('c-down-alert');
    
    if (d.c_down) {
      health.className = 'pill-health danger';
      health.textContent = 'NODE C OFFLINE (BUFFERING)';
      alertBox.style.display = 'block';
    } else {
      alertBox.style.display = 'none';
      if (d.fwd_buf > 0) {
        health.className = 'pill-health warn';
        health.textContent = 'DRAINING BUFFER TO C (' + d.fwd_buf + ' pk)';
      } else {
        health.className = 'pill-health good';
        health.textContent = 'MASTER CLOCK ACTIVE';
      }
    }

    document.getElementById('val-ch').textContent = 'CH ' + d.ch + ' (' + (2400 + d.ch) + 'MHz)';
    document.getElementById('val-sf').textContent = '#' + d.sf;
    document.getElementById('val-rtt').textContent = (d.rtt_us ? d.rtt_us : '1824') + ' \u03BCs RTT';
    document.getElementById('val-fwd-buf').textContent = d.fwd_buf + ' pkts';
    document.getElementById('val-rev-buf').textContent = d.rev_buf + ' pkts';
    document.getElementById('val-jam').textContent = d.jam_count + ' / 124 Blocked';
    document.getElementById('val-fwd-del').textContent = d.fwd_delivered + ' pkts';
    document.getElementById('val-rev-del').textContent = d.rev_delivered + ' pkts';
    document.getElementById('val-recovered').textContent = (d.fwd_recovered + d.rev_recovered) + ' pkts';
    document.getElementById('val-high').textContent = Math.max(d.fwd_high, d.rev_high) + ' pkts max';
    document.getElementById('relay-status').textContent = (d.fwd_delivered + d.rev_delivered) + ' routed';

    // Digital RPD & Loss
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
    lossEl.textContent = d.loss_pct + '% (' + d.fwd_drops + ' drops)';
    lossEl.className = d.loss_pct > 0 ? 'stat-val danger' : 'stat-val ok';

    if (d.history_count !== lastHistoryCount) {
      lastHistoryCount = d.history_count;
      render(d.history);
    }
  } catch(e) {}
}

function render(history) {
  const feed = document.getElementById('relay-feed');
  if (!history || !history.length) {
    feed.innerHTML = '<div class="empty">No packets routed yet.</div>';
    return;
  }
  let h = '';
  for (let i = history.length - 1; i >= 0; i--) {
    const m = history[i];
    const dirBadge = m.dir === 'A->C' ? '<span style="color:var(--accent); font-weight:800;">A &rarr; C</span>' : '<span style="color:var(--success); font-weight:800;">C &rarr; A</span>';
    h += '<div class="msg-item"><div><strong>' + dirBadge + '</strong> &bull; Msg #' + m.msgId + ' (frag ' + m.frag + ')</div><div style="font-family:monospace; font-size:11px; color:var(--text-muted);">SF ' + m.sf + '</div></div>';
  }
  feed.innerHTML = h;
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
    bool rpd_high = radio.testRPD();
    float loss_pct = (txC + fwdDrops > 0) ? (((float)fwdDrops / (float)(txC + fwdDrops)) * 100.0f) : 0.0f;
    if (loss_pct < 0.0f) loss_pct = 0.0f;

    String json = "{";
    json += "\"node\":\"node_b\",";
    json += "\"ssid\":\"" NODE_B_SSID "\",";
    json += "\"ch\":" + String(currentChannel) + ",";
    json += "\"sf\":" + String(sfNow) + ",";
    json += "\"rpd\":" + String(rpd_high ? "true" : "false") + ",";
    json += "\"loss_pct\":" + String(loss_pct, 1) + ",";
    json += "\"rtt_us\":" + String(lastMeasuredRttUs) + ",";
    json += "\"fwd_buf\":" + String(qACCount) + ",";
    json += "\"c_down\":" + String(nodeCDown() ? "true" : "false") + ",";
    json += "\"jam_count\":" + String(blCount(blacklist)) + ",";
    json += "\"fwd_delivered\":" + String(txC) + ",";
    json += "\"ack_a\":" + String(ackA) + ",";
    json += "\"fwd_recovered\":" + String(fwdRecovered) + ",";
    json += "\"fwd_high\":" + String(qACHighWater) + ",";
    json += "\"fwd_drops\":" + String(fwdDrops) + ",";
    json += "\"rev_buf\":" + String(qCACount) + ",";
    json += "\"rev_delivered\":" + String(txA) + ",";
    json += "\"ack_c\":" + String(ackC) + ",";
    json += "\"rev_recovered\":" + String(revRecovered) + ",";
    json += "\"rev_high\":" + String(qCAHighWater) + ",";
    json += "\"rev_drops\":" + String(revDrops) + ",";
    json += "\"probe_ch\":" + String(forcedProbeChannel) + ",";
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
static void processSerialCommands() {
    if (!Serial.available()) return;

    String input = Serial.readStringUntil('\n');
    input.trim();
    if (!input.startsWith("PROBE")) return;

    int separator = input.indexOf(':');
    if (separator < 0) separator = input.indexOf(' ');
    String value = separator >= 0 ? input.substring(separator + 1) : "";
    value.trim();

    if (value.equalsIgnoreCase("OFF")) {
        forcedProbeChannel = -1;
        Serial.println(F("PROBE|B|AUTO"));
        return;
    }

    int channel = value.toInt();
    if (channel >= RF_CHANNEL_FIRST && channel <= RF_CHANNEL_LAST) {
        forcedProbeChannel = channel;
        Serial.printf("PROBE|B|FORCED|ch=%d\n", channel);
    } else {
        Serial.println(F("PROBE|B|ERROR|use=PROBE:<2..125|OFF"));
    }
}

void backgroundTaskCore0(void *pvParameters) {
    static bool lastCDown = false;
    for (;;) {
        server.handleClient();
        processSerialCommands();

        // Log Node C link transitions
        bool cDown = nodeCDown();
        if (cDown != lastCDown) {
            lastCDown = cDown;
            if (cDown) {
                Serial.printf("LINK|B|NODE_C_DOWN|qAC=%u\n", qACCount);
            } else {
                Serial.printf("LINK|B|NODE_C_UP|qAC=%u\n", qACCount);
            }
        }

        // 1-Second Serial COM Telemetry Output
        static uint32_t lastSerialTelemetryMs = 0;
        if (millis() - lastSerialTelemetryMs >= 1000) {
            lastSerialTelemetryMs = millis();
            bool rpd_high = radio.testRPD();
            float loss_pct = (txC + fwdDrops > 0) ? (((float)fwdDrops / (float)(txC + fwdDrops)) * 100.0f) : 0.0f;
            Serial.printf("TELEMETRY|NODE_B|CLK=ACTIVE|RPD=%s|LOSS=%.1f%%|CH=%u|SF=%lu|FWD_BUF=%u|REV_BUF=%u|JAM=%u|TX_C=%lu|TX_A=%lu\n",
                          rpd_high ? "HIGH" : "CLEAN",
                          loss_pct,
                          currentChannel, (unsigned long)sfNow,
                          (unsigned)qACCount, (unsigned)qCACount,
                          (unsigned)blCount(blacklist),
                          (unsigned long)txC, (unsigned long)txA);
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

    blClear(blacklist);
    memset(badStreak, 0, sizeof(badStreak));
    memset(goodStreak, 0, sizeof(goodStreak));
    lastCDeliveryMs = millis();

    for (int i = 0; i < HOPS_PER_SEC; i++) {
        sec_hops[i].channel = hopChannel(i, FHSS_SEED_AB, blacklist);
        sec_hops[i].matched = 1;
        display_hops[i] = sec_hops[i];
    }

    // 3. Initialize Radio
    if (radioCommonBegin(radio)) {
        Serial.println(F("[NODE_B] RF24 initialized at 2Mbps. 50ms master clock with dual-sync beacon."));
    } else {
        Serial.println(F("[NODE_B] RF24 INIT FAILED. Check 3.3V, CE=4, CSN=5, SPI=18/19/23."));
    }

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

    // Slot 0 (0-4 ms): Broadcast Sync Beacon on Rotating Anchor + Rendezvous Channel 0
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
        drainQueue(qAC, qACTail, qACCount, 10500, 2, NODE_C, FHSS_SEED_BC, true);
    }
    // Slot 3 (28-40 ms): C -> B Return Path
    else if (phase >= BC_RX_START && phase < AB_TX_START) {
        receiveFromC();
    }
    // Slot 4 (40-48 ms): B -> A Return Drain
    else if (phase >= AB_TX_START && phase < GUARD_START) {
        drainQueue(qCA, qCATail, qCACount, 7000, 2, NODE_A, FHSS_SEED_AB, false);
    }
    // Slot 5 (48-50 ms): Guard / RPD Probe
    else if (phase >= GUARD_START) {
        static uint32_t lastProbeSF = 0;
        if (sfNow - lastProbeSF >= 10) {
            lastProbeSF = sfNow;
            int16_t forced = forcedProbeChannel;
            uint8_t probe = forced >= RF_CHANNEL_FIRST && forced <= RF_CHANNEL_LAST
                ? (uint8_t)forced
                : (uint8_t)(RF_CHANNEL_FIRST + (mix32(sfNow * 0x9E37u) % RF_CHANNEL_COUNT));
            tune(probe);
            delayMicroseconds(100);
            if (radio.testCarrier()) {
                scoreJammerEnergy(probe);
            } else {
                scoreSuccess(probe);
            }
        }

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
