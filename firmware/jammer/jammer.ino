// HopperNet Jammer — nRF24L01+ RF jammer on FHSS channels 2–125
//
// Uses a dedicated nRF24L01+ module to transmit on random channels within the
// FHSS band.  The raw RF energy triggers testCarrier() / RPD on nearby
// receivers, forcing the mesh to blacklist channels and hop away.
//
// Wiring (ESP32): CE=GPIO25, CSN=GPIO26, SCK=18, MOSI=23, MISO=19
// (same SPI bus as Node A but different CE/CSN — don't run both on one ESP32)
//
// Serial commands (115200 baud):
//   j        — toggle jamming ON/OFF
//   c <2-125> — lock to a specific FHSS channel
//   d <ms>   — set dwell time per channel (default 50 ms)
//   p <0-3>  — set PA level (0=LOW, 1=MED, 2=HIGH, 3=MAX, default MAX)
//   s        — print status
//   r        — resume random hopping
//   b        — broadcast jam on ALL 124 channels (sweep)

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define CE_PIN   25
#define CSN_PIN  26

#define CHANNEL_BASE  2
#define NUM_CHANNELS  124
#define CHANNEL_MAX   (CHANNEL_BASE + NUM_CHANNELS - 1)  // 125

RF24 radio(CE_PIN, CSN_PIN);

bool jamming = false;
int currentChannel = CHANNEL_BASE;
bool lockChannel = false;
int dwellTime = 50;
unsigned long txCount = 0;
uint8_t paLevel = RF24_PA_MAX;
int paIndex = 3;  // MAX

static const char* PA_NAMES[] = {"LOW", "MED", "HIGH", "MAX"};
static const rf24_pa_dbm_e PA_VALS[] = {RF24_PA_LOW, RF24_PA_MED, RF24_PA_HIGH, RF24_PA_MAX};

// 32-byte dummy payload (max nRF24L01+ payload)
static uint8_t junk[32];

void setChannel(int ch) {
  if (ch < CHANNEL_BASE) ch = CHANNEL_BASE;
  if (ch > CHANNEL_MAX) ch = CHANNEL_MAX;
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
  // fill junk with random bytes
  for (int i = 0; i < 32; i++) junk[i] = random(0x00, 0xFF);
  Serial.printf("[JAMMER] ON  ch=%d  PA=%s  dwell=%d ms\n",
                currentChannel, PA_NAMES[paIndex], dwellTime);
}

void stopJammer() {
  radio.startListening();
  Serial.printf("[JAMMER] OFF (tx=%lu)\n", (unsigned long)txCount);
}

void sweepAllChannels() {
  Serial.println("[JAMMER] Sweep mode — hitting all 124 channels...");
  for (int ch = CHANNEL_BASE; ch <= CHANNEL_MAX; ch++) {
    radio.setChannel(ch);
    for (int i = 0; i < 5; i++) {
      // randomise junk each sweep
      junk[random(0, 32)] = random(0x00, 0xFF);
      radio.writeFast(junk, 32);
      txCount++;
    }
  }
  Serial.printf("[JAMMER] Sweep done (tx=%lu)\n", (unsigned long)txCount);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(0));

  if (!radio.begin()) {
    Serial.println("[JAMMER] RF24 init FAILED — check wiring");
    while (1) delay(100);
  }

  Serial.println("==========================================");
  Serial.println("  HopperNet RF Jammer  v3.0");
  Serial.println("  nRF24L01+ on FHSS channels 2–125");
  Serial.println("==========================================");
  Serial.println("Commands:");
  Serial.println("  j          — toggle jamming ON/OFF");
  Serial.println("  c <2-125>  — lock to FHSS channel");
  Serial.println("  d <ms>     — dwell time (default 50)");
  Serial.println("  p <0-3>    — PA level (0=LOW 3=MAX)");
  Serial.println("  s          — status");
  Serial.println("  r          — resume random hopping");
  Serial.println("  b          — sweep ALL 124 channels");
  Serial.println("==========================================");
  Serial.println("Ready. Type 'j' to start jamming.");
}

void loop() {
  static unsigned long lastHop = 0;

  if (jamming && !lockChannel && millis() - lastHop > (unsigned long)dwellTime) {
    currentChannel = random(CHANNEL_BASE, CHANNEL_MAX + 1);
    radio.setChannel(currentChannel);
    // transmit 3 junk packets per hop
    for (int i = 0; i < 3; i++) {
      junk[random(0, 32)] = random(0x00, 0xFF);
      radio.writeFast(junk, 32);
      txCount++;
    }
    lastHop = millis();
  }

  if (jamming && lockChannel) {
    // continuous TX on locked channel
    if (millis() - lastHop > (unsigned long)dwellTime) {
      for (int i = 0; i < 5; i++) {
        junk[random(0, 32)] = random(0x00, 0xFF);
        radio.writeFast(junk, 32);
        txCount++;
      }
      lastHop = millis();
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
        if (ch >= CHANNEL_BASE && ch <= CHANNEL_MAX) {
          currentChannel = ch;
          lockChannel = true;
          if (jamming) {
            radio.setChannel(currentChannel);
          }
          Serial.printf("[JAMMER] Locked to channel %d\n", currentChannel);
        } else {
          Serial.println("[JAMMER] Invalid channel (2-125)");
        }
        break;
      }
      case 'd': {
        int ms = Serial.parseInt();
        if (ms > 0) {
          dwellTime = ms;
          Serial.printf("[JAMMER] Dwell: %d ms\n", dwellTime);
        }
        break;
      }
      case 'p': {
        int p = Serial.parseInt();
        if (p >= 0 && p <= 3) {
          paIndex = p;
          paLevel = PA_VALS[paIndex];
          if (jamming) radio.setPALevel(paLevel);
          Serial.printf("[JAMMER] PA: %s\n", PA_NAMES[paIndex]);
        }
        break;
      }
      case 's':
        Serial.printf("[JAMMER] %s | ch=%s%d | PA=%s | dwell=%d ms | tx=%lu\n",
                       jamming ? "JAMMING" : "IDLE",
                       lockChannel ? "" : "~",
                       currentChannel, PA_NAMES[paIndex], dwellTime,
                       (unsigned long)txCount);
        break;
      case 'r':
        lockChannel = false;
        Serial.println("[JAMMER] Random hopping resumed");
        break;
      case 'b':
        sweepAllChannels();
        break;
    }
  }
}