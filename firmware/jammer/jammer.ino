// HopperNet RF Jammer & Adversary Console (Arduino Mega 2560)
// Hardware: Arduino Mega 2560 + nRF24L01+ + Optional 3.5" Touchscreen Shield

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "fhss.h"

// ---------------- Hardware & Pin Config (Arduino Mega 2560) ----------------
#define CE_PIN          9
#define CSN_PIN         53
#define BAUD            115200

#define CHANNEL_BASE    2
#define NUM_CHANNELS    124

RF24 radio(CE_PIN, CSN_PIN);

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
    Serial.print(F("[JAMMER] ACTIVE | mode="));
    Serial.print(mode);
    Serial.print(F(" | ch="));
    Serial.print(currentChannel);
    Serial.print(F(" | PA="));
    Serial.print(PA_NAMES[paIndex]);
    Serial.print(F(" | dwell="));
    Serial.print(dwellTime);
    Serial.println(F(" ms"));
}

void stopJammer() {
    radio.startListening();
    Serial.print(F("[JAMMER] STOPPED | total tx="));
    Serial.println(txCount);
}

void sweepAllChannels() {
    Serial.println(F("[JAMMER] Sweeping all 124 channels..."));
    for (int ch = CHANNEL_BASE; ch < CHANNEL_BASE + NUM_CHANNELS; ch++) {
        radio.setChannel(ch);
        for (int i = 0; i < 4; i++) {
            junk[random(0, 32)] = random(0x00, 0xFF);
            radio.writeFast(junk, 32);
            txCount++;
        }
    }
    Serial.print(F("[JAMMER] Sweep complete (tx="));
    Serial.print(txCount);
    Serial.println(F(")"));
}

void printStatus() {
    Serial.print(F("[JAMMER] "));
    Serial.print(jamming ? F("JAMMING") : F("IDLE"));
    Serial.print(F(" | Mode: "));
    Serial.print(mode);
    Serial.print(F(" | Ch: "));
    Serial.print(currentChannel);
    Serial.print(F(" | PA: "));
    Serial.print(PA_NAMES[paIndex]);
    Serial.print(F(" | Dwell: "));
    Serial.print(dwellTime);
    Serial.print(F(" ms | TX: "));
    Serial.println(txCount);
}

void setup() {
    Serial.begin(BAUD);
    delay(1000);
    randomSeed(analogRead(0));

    if (!radio.begin()) {
        Serial.println(F("[JAMMER] RF24 init FAILED — check wiring (Mega pins 50-53, CE=9, CSN=53)"));
        while (1) delay(100);
    }

    Serial.println(F("=========================================="));
    Serial.println(F("  HopperNet Adversary & RF Jammer (Mega)  "));
    Serial.println(F("  2.4 GHz ISM Spectrum (Channels 2–125)   "));
    Serial.println(F("=========================================="));
    Serial.println(F("Commands (Serial or Touchscreen):"));
    Serial.println(F("  j          — Toggle Jamming ON / OFF"));
    Serial.println(F("  c <2-125>  — Lock onto specific RF Channel"));
    Serial.println(F("  r          — Random Channel Hopping Mode"));
    Serial.println(F("  a          — Adaptive Mesh-Tracking Mode"));
    Serial.println(F("  b          — Instant Full-Band 124-Channel Sweep"));
    Serial.println(F("  d <ms>     — Set Dwell Duration (ms)"));
    Serial.println(F("  p <0-3>    — Set Power (0:MIN, 1:LOW, 2:HIGH, 3:MAX)"));
    Serial.println(F("  s          — Print Status Summary"));
    Serial.println(F("=========================================="));
    Serial.println(F("Ready. Type 'j' to start."));
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
        } else if (mode == JAM_ADAPTIVE && millis() - lastAction > 25) {
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
                    Serial.print(F("[JAMMER] Locked to channel "));
                    Serial.println(currentChannel);
                }
                break;
            }
            case 'r':
                mode = JAM_RANDOM;
                Serial.println(F("[JAMMER] Switched to Random Hopping Mode"));
                break;
            case 'a':
                mode = JAM_ADAPTIVE;
                simulated_hop = micros() / DWELL_US;
                Serial.println(F("[JAMMER] Switched to Adaptive Mesh-Tracking Mode"));
                break;
            case 'b':
                sweepAllChannels();
                break;
            case 'd': {
                int ms = Serial.parseInt();
                if (ms > 0) {
                    dwellTime = ms;
                    Serial.print(F("[JAMMER] Dwell set to "));
                    Serial.print(dwellTime);
                    Serial.println(F(" ms"));
                }
                break;
            }
            case 'p': {
                int p = Serial.parseInt();
                if (p >= 0 && p <= 3) {
                    paIndex = p;
                    paLevel = PA_VALS[paIndex];
                    if (jamming) radio.setPALevel(paLevel);
                    Serial.print(F("[JAMMER] PA level set to "));
                    Serial.println(PA_NAMES[paIndex]);
                }
                break;
            }
            case 's':
                printStatus();
                break;
        }
    }
}