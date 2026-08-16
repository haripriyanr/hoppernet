// HopperNet RF Jammer & Interference Simulator — ESP32 + nRF24L01+
// Supports Random Hopping, Continuous Lock, Sweeping, Targeted, and Adaptive Modes.

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "fhss.h"
#include "fhss_config.h"

RF24 radio(JAMMER_CE_PIN, JAMMER_CSN_PIN);

enum JammerMode {
    JAM_RANDOM,
    JAM_LOCKED,
    JAM_ADAPTIVE,
    JAM_SWEEP
};

bool jamming = false;
JammerMode mode = JAM_RANDOM;
int currentChannel = CHANNEL_BASE;
int dwellTime = 50;
unsigned long txCount = 0;
uint8_t paLevel = RF24_PA_MAX;
int paIndex = 3;  // MAX

static const char* PA_NAMES[] = {"MIN", "LOW", "HIGH", "MAX"};
static const rf24_pa_dbm_e PA_VALS[] = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};

static uint8_t junk[32];
static uint32_t simulated_hop = 0;
static uint8_t dummy_empty_blacklist[BLACKLIST_SIZE] = {0};

void setChannel(int ch) {
    if (ch < CHANNEL_BASE) ch = CHANNEL_BASE;
    if (ch > CHANNEL_BASE + NUM_CHANNELS - 1) ch = CHANNEL_BASE + NUM_CHANNELS - 1;
    currentChannel = ch;
    radio.setChannel(currentChannel);
}

void startJammer() {
    radio.stopListening();
    radio.setPALevel(PA_VALS[paIndex]);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(32);
    radio.setAutoAck(false);
    radio.setChannel(currentChannel);
    for (int i = 0; i < 32; i++) junk[i] = random(0x00, 0xFF);
    Serial.printf("[JAMMER] ACTIVE | mode=%d | ch=%d | PA=%s | dwell=%d ms\n",
                  mode, currentChannel, PA_NAMES[paIndex], dwellTime);
}

void stopJammer() {
    radio.startListening();
    Serial.printf("[JAMMER] STOPPED | total tx=%lu\n", (unsigned long)txCount);
}

void sweepAllChannels() {
    Serial.println("[JAMMER] Sweeping all 124 channels...");
    for (int ch = CHANNEL_BASE; ch < CHANNEL_BASE + NUM_CHANNELS; ch++) {
        radio.setChannel(ch);
        for (int i = 0; i < 4; i++) {
            junk[random(0, 32)] = random(0x00, 0xFF);
            radio.writeFast(junk, 32);
            txCount++;
        }
    }
    Serial.printf("[JAMMER] Sweep complete (tx=%lu)\n", (unsigned long)txCount);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    randomSeed(analogRead(0));

    if (!radio.begin()) {
        Serial.println("[JAMMER] RF24 initialization FAILED — verify wiring!");
        while (1) delay(100);
    }

    Serial.println("==========================================");
    Serial.println("  HopperNet Adversary & RF Jammer v4.0   ");
    Serial.println("  2.4 GHz ISM Spectrum (Channels 2–125)   ");
    Serial.println("==========================================");
    Serial.println("Commands:");
    Serial.println("  j          — Toggle Jamming ON / OFF");
    Serial.println("  c <2-125>  — Lock onto specific RF Channel");
    Serial.println("  r          — Random Channel Hopping Mode");
    Serial.println("  a          — Adaptive Mesh-Tracking Mode");
    Serial.println("  b          — Instant Full-Band 124-Channel Sweep");
    Serial.println("  d <ms>     — Set Dwell Duration (ms)");
    Serial.println("  p <0-3>    — Set Power Level (0:LOW, 3:MAX)");
    Serial.println("  s          — Status Summary");
    Serial.println("==========================================");
    Serial.println("Ready. Type 'j' to start.");
}

void loop() {
    static unsigned long lastAction = 0;

    if (jamming) {
        if (mode == JAM_RANDOM && millis() - lastAction > (unsigned long)dwellTime) {
            currentChannel = random(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS);
            radio.setChannel(currentChannel);
            for (int i = 0; i < 3; i++) {
                junk[random(0, 32)] = random(0x00, 0xFF);
                radio.writeFast(junk, 32);
                txCount++;
            }
            lastAction = millis();
        } else if (mode == JAM_LOCKED && millis() - lastAction > (unsigned long)dwellTime) {
            for (int i = 0; i < 5; i++) {
                junk[random(0, 32)] = random(0x00, 0xFF);
                radio.writeFast(junk, 32);
                txCount++;
            }
            lastAction = millis();
        } else if (mode == JAM_ADAPTIVE && millis() - lastAction > 25) { // Tracks 25ms mesh dwell
            simulated_hop++;
            currentChannel = channel_for_hop(simulated_hop, FHSS_SEED, dummy_empty_blacklist);
            radio.setChannel(currentChannel);
            for (int i = 0; i < 4; i++) {
                junk[random(0, 32)] = random(0x00, 0xFF);
                radio.writeFast(junk, 32);
                txCount++;
            }
            lastAction = millis();
        }
    }

    if (Serial.available()) {
        char cmd = Serial.read();
        switch (cmd) {
            case 'j':
                jamming = !jamming;
                if (jamming) startJammer();
                else stopJammer();
                break;
            case 'c': {
                int ch = Serial.parseInt();
                if (ch >= CHANNEL_BASE && ch < CHANNEL_BASE + NUM_CHANNELS) {
                    setChannel(ch);
                    mode = JAM_LOCKED;
                    Serial.printf("[JAMMER] Locked to channel %d\n", currentChannel);
                }
                break;
            }
            case 'r':
                mode = JAM_RANDOM;
                Serial.println("[JAMMER] Switched to Random Hopping Mode");
                break;
            case 'a':
                mode = JAM_ADAPTIVE;
                simulated_hop = micros() / DWELL_US;
                Serial.println("[JAMMER] Switched to Adaptive Mesh-Tracking Mode");
                break;
            case 'b':
                sweepAllChannels();
                break;
            case 'd': {
                int ms = Serial.parseInt();
                if (ms > 0) {
                    dwellTime = ms;
                    Serial.printf("[JAMMER] Dwell set to %d ms\n", dwellTime);
                }
                break;
            }
            case 'p': {
                int p = Serial.parseInt();
                if (p >= 0 && p <= 3) {
                    paIndex = p;
                    paLevel = PA_VALS[paIndex];
                    if (jamming) radio.setPALevel(paLevel);
                    Serial.printf("[JAMMER] PA level set to %s\n", PA_NAMES[paIndex]);
                }
                break;
            }
            case 's':
                Serial.printf("[JAMMER] %s | Mode: %d | Ch: %d | PA: %s | Dwell: %d ms | TX: %lu\n",
                              jamming ? "JAMMING" : "IDLE", mode, currentChannel,
                              PA_NAMES[paIndex], dwellTime, (unsigned long)txCount);
                break;
        }
    }
}