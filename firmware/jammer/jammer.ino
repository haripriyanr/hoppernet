// HopperNet RF Jammer & Adversary Console (Arduino Mega 2560)
// Hardware: Arduino Mega 2560 + nRF24L01+ + 3.5" TFT Touchscreen Shield (ILI9486)
// Touchscreen Pinout: YP=A2, XM=A3, YM=8, XP=9
// Radio Pinout (Rear SPI): CE=43, CSN=45, SCK=52, MOSI=51, MISO=50

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include "fhss.h"

// ---------------- Hardware & Pin Configuration ----------------
#define CE_PIN          43
#define CSN_PIN         45
#define BAUD            115200

#define YP A2  // Must be an analog pin
#define XM A3  // Must be an analog pin
#define YM 8   // Digital pin
#define XP 9   // Digital pin

#define TS_MINX 100
#define TS_MAXX 920
#define TS_MINY 120
#define TS_MAXY 940

#define MINPRESSURE 10
#define MAXPRESSURE 1000

MCUFRIEND_kbv tft;
TouchScreen ts = TouchScreen(XP, YP, XM, YM, 300);
RF24 radio(CE_PIN, CSN_PIN);

// ---------------- Color Palette (16-bit 565) ----------------
#define COLOR_BG        0x0842  // Very dark slate blue
#define COLOR_HEADER    0x10A4  // Dark slate
#define COLOR_CARD      0x18E7  // Panel grey-blue
#define COLOR_BORDER    0x31A6  // Border grey
#define COLOR_TEXT      0xFFFF  // White
#define COLOR_MUTED     0x9CF3  // Light grey
#define COLOR_ACCENT    0x07FF  // Cyan
#define COLOR_RED       0xF986  // Vivid Red
#define COLOR_GREEN     0x07E0  // Vivid Green
#define COLOR_AMBER     0xFDA0  // Amber/Yellow
#define COLOR_BLUE      0x2C1F  // Deep Blue
#define COLOR_DARK      0x0000  // Black
#define COLOR_ACTIVE    0xF81F  // Magenta

// ---------------- Jammer Modes ----------------
enum JammerMode {
    JAM_SPOT = 0,     // Single channel continuous jamming
    JAM_SWEEP,        // Rapid sweep of all 124 channels
    JAM_RANDOM,       // Random hopping per dwell
    JAM_AUTOHOP,      // Mirror FHSS algorithm (synchronized tracking)
    JAM_ADAPTIVE,     // Sniff live carrier energy & target hot channels
    JAM_BURST,        // High-rate microburst pulse jamming
    JAM_ANCHOR        // Target PMER Rendezvous Anchors {10, 42, 74, 106}
};

static const char* MODE_NAMES[] = {
    "SPOT", "SWEEP", "RANDOM", "AUTO-HOP", "ADAPTIVE", "BURST", "ANCHOR"
};

// Dwell presets (ms)
static const int DWELL_PRESETS[] = {5, 10, 15, 25, 50, 100, 200, 500};
#define NUM_DWELL_PRESETS 8
static int dwellPresetIdx = 3; // Default 25ms

// PA Levels
static const char* PA_NAMES[] = {"MIN", "LOW", "HIGH", "MAX"};
static const rf24_pa_dbm_e PA_VALS[] = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};
static int paIndex = 3; // MAX

// ---------------- State Variables ----------------
bool jamming = false;
bool paused = false;
JammerMode mode = JAM_SPOT;
int currentChannel = 45;
int dwellTime = 25;

unsigned long txCount = 0;
unsigned long lastTxRateCheck = 0;
unsigned long txCountSnapshot = 0;
unsigned int currentTxRate = 0;

static uint8_t junk[32];
static uint32_t simulated_hop = 0;
static uint8_t dummy_empty_blacklist[BLACKLIST_SIZE] = {0};
static uint8_t anchorIdx = 0;
static uint8_t sweepCh = CHANNEL_BASE;

// UI State
uint16_t screenID = 0;
unsigned long lastTouchTime = 0;
unsigned long lastUiUpdate = 0;
bool touchDebounce = false;

// ---------------- Touchscreen Pin Restoration ----------------
void restoreLcdPins() {
    pinMode(A0, OUTPUT);
    pinMode(A1, OUTPUT);
    pinMode(A2, OUTPUT);
    pinMode(A3, OUTPUT);
    pinMode(A4, OUTPUT);
    pinMode(8, OUTPUT);
    pinMode(9, OUTPUT);
    pinMode(6, OUTPUT);
    pinMode(7, OUTPUT);
}

// ---------------- Radio Control ----------------
void setChannel(int ch) {
    if (ch < CHANNEL_BASE) ch = CHANNEL_BASE;
    if (ch > CHANNEL_BASE + NUM_CHANNELS - 1) ch = CHANNEL_BASE + NUM_CHANNELS - 1;
    currentChannel = ch;
    radio.setChannel(currentChannel);
}

void stepChannel(int delta) {
    int newCh = currentChannel + delta;
    if (newCh < CHANNEL_BASE) newCh = CHANNEL_BASE + NUM_CHANNELS - 1;
    if (newCh > CHANNEL_BASE + NUM_CHANNELS - 1) newCh = CHANNEL_BASE;
    setChannel(newCh);
}

void stepDwell(int delta) {
    dwellPresetIdx += delta;
    if (dwellPresetIdx < 0) dwellPresetIdx = 0;
    if (dwellPresetIdx >= NUM_DWELL_PRESETS) dwellPresetIdx = NUM_DWELL_PRESETS - 1;
    dwellTime = DWELL_PRESETS[dwellPresetIdx];
}

void stepPower(int delta) {
    paIndex += delta;
    if (paIndex < 0) paIndex = 0;
    if (paIndex > 3) paIndex = 3;
    radio.setPALevel(PA_VALS[paIndex]);
}

void startJammer() {
    jamming = true;
    paused = false;
    radio.stopListening();
    radio.setPALevel(PA_VALS[paIndex]);
    radio.setDataRate(RF24_250KBPS);
    radio.setPayloadSize(32);
    radio.setAutoAck(false);
    radio.setChannel(currentChannel);
    for (int i = 0; i < 32; i++) junk[i] = random(0x00, 0xFF);
    Serial.print(F("[JAMMER] STARTED | Mode: "));
    Serial.print(MODE_NAMES[mode]);
    Serial.print(F(" | Ch: "));
    Serial.print(currentChannel);
    Serial.print(F(" | PA: "));
    Serial.print(PA_NAMES[paIndex]);
    Serial.print(F(" | Dwell: "));
    Serial.print(dwellTime);
    Serial.println(F(" ms"));
}

void stopJammer() {
    jamming = false;
    paused = false;
    radio.startListening();
    Serial.print(F("[JAMMER] STOPPED | Total TX: "));
    Serial.println(txCount);
}

void togglePause() {
    if (!jamming) return;
    paused = !paused;
    if (paused) {
        radio.startListening();
        Serial.println(F("[JAMMER] PAUSED (RF Output Suspended)"));
    } else {
        radio.stopListening();
        Serial.println(F("[JAMMER] RESUMED"));
    }
}

// ---------------- RF Transmit Utilities ----------------
inline void blastPackets(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        junk[random(0, 32)] = (uint8_t)random(0x00, 0xFF);
        radio.writeFast(junk, 32);
        txCount++;
    }
}

// ---------------- UI Drawing Functions ----------------
void drawHeader() {
    tft.fillRect(0, 0, tft.width(), 38, COLOR_HEADER);
    tft.drawFastHLine(0, 38, tft.width(), COLOR_ACCENT);

    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(12, 11);
    tft.print(F("RF JAMMER CONSOLE"));

    // Status Pill
    uint16_t pillBg = jamming ? (paused ? COLOR_AMBER : COLOR_RED) : COLOR_CARD;
    uint16_t pillText = jamming ? (paused ? COLOR_DARK : COLOR_TEXT) : COLOR_MUTED;
    tft.fillRoundRect(355, 6, 115, 26, 6, pillBg);
    tft.drawRoundRect(355, 6, 115, 26, 6, COLOR_TEXT);
    tft.setTextColor(pillText, pillBg);
    tft.setTextSize(1);
    tft.setCursor(368, 15);
    if (!jamming) {
        tft.print(F("STATE: IDLE"));
    } else if (paused) {
        tft.print(F("STATE: PAUSED"));
    } else {
        tft.print(F("STATE: JAMMING"));
    }
}

void drawModeButtons() {
    // Mode Buttons: 7 buttons laid out in 2 rows
    // Row 1: SPOT, SWEEP, RANDOM, AUTO-HOP
    // Row 2: ADAPTIVE, BURST, ANCHOR
    int btnW1 = 110;
    int btnH = 32;
    int startY1 = 44;

    for (int i = 0; i < 4; i++) {
        int x = 10 + i * (btnW1 + 8);
        bool isActive = (mode == (JammerMode)i);
        uint16_t bg = isActive ? COLOR_RED : COLOR_CARD;
        uint16_t border = isActive ? COLOR_TEXT : COLOR_BORDER;
        uint16_t text = isActive ? COLOR_TEXT : COLOR_MUTED;

        tft.fillRoundRect(x, startY1, btnW1, btnH, 6, bg);
        tft.drawRoundRect(x, startY1, btnW1, btnH, 6, border);
        tft.setTextColor(text, bg);
        tft.setTextSize(1);
        tft.setCursor(x + 12, startY1 + 12);
        tft.print(MODE_NAMES[i]);
    }

    int btnW2 = 148;
    int startY2 = 82;
    for (int i = 0; i < 3; i++) {
        int x = 10 + i * (btnW2 + 9);
        int modeIdx = i + 4;
        bool isActive = (mode == (JammerMode)modeIdx);
        uint16_t bg = isActive ? COLOR_RED : COLOR_CARD;
        uint16_t border = isActive ? COLOR_TEXT : COLOR_BORDER;
        uint16_t text = isActive ? COLOR_TEXT : COLOR_MUTED;

        tft.fillRoundRect(x, startY2, btnW2, btnH, 6, bg);
        tft.drawRoundRect(x, startY2, btnW2, btnH, 6, border);
        tft.setTextColor(text, bg);
        tft.setTextSize(1);
        tft.setCursor(x + 16, startY2 + 12);
        tft.print(MODE_NAMES[modeIdx]);
    }
}

void drawSteppers() {
    int cardY = 120;
    int cardH = 68;

    // --- 1. CHANNEL STEPPER ---
    int cardX1 = 10;
    int cardW = 148;
    tft.fillRoundRect(cardX1, cardY, cardW, cardH, 6, COLOR_CARD);
    tft.drawRoundRect(cardX1, cardY, cardW, cardH, 6, COLOR_BORDER);
    tft.setTextColor(COLOR_ACCENT, COLOR_CARD);
    tft.setTextSize(1);
    tft.setCursor(cardX1 + 10, cardY + 8);
    tft.print(F("CHANNEL (2-125)"));

    // Decrement Button [-]
    tft.fillRoundRect(cardX1 + 8, cardY + 26, 34, 32, 4, COLOR_HEADER);
    tft.drawRoundRect(cardX1 + 8, cardY + 26, 34, 32, 4, COLOR_ACCENT);
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(cardX1 + 18, cardY + 34);
    tft.print(F("-"));

    // Channel Value Display
    tft.fillRect(cardX1 + 46, cardY + 26, 56, 32, COLOR_BG);
    tft.drawRect(cardX1 + 46, cardY + 26, 56, 32, COLOR_BORDER);
    tft.setTextColor(COLOR_AMBER, COLOR_BG);
    tft.setTextSize(2);
    tft.setCursor(cardX1 + 54, cardY + 34);
    if (currentChannel < 10) tft.print(F("00"));
    else if (currentChannel < 100) tft.print(F("0"));
    tft.print(currentChannel);

    // Increment Button [+]
    tft.fillRoundRect(cardX1 + 106, cardY + 26, 34, 32, 4, COLOR_HEADER);
    tft.drawRoundRect(cardX1 + 106, cardY + 26, 34, 32, 4, COLOR_ACCENT);
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(cardX1 + 116, cardY + 34);
    tft.print(F("+"));

    // --- 2. DWELL STEPPER ---
    int cardX2 = 166;
    tft.fillRoundRect(cardX2, cardY, cardW, cardH, 6, COLOR_CARD);
    tft.drawRoundRect(cardX2, cardY, cardW, cardH, 6, COLOR_BORDER);
    tft.setTextColor(COLOR_ACCENT, COLOR_CARD);
    tft.setTextSize(1);
    tft.setCursor(cardX2 + 10, cardY + 8);
    tft.print(F("DWELL TIME"));

    // Decrement Button [-]
    tft.fillRoundRect(cardX2 + 8, cardY + 26, 34, 32, 4, COLOR_HEADER);
    tft.drawRoundRect(cardX2 + 8, cardY + 26, 34, 32, 4, COLOR_ACCENT);
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(cardX2 + 18, cardY + 34);
    tft.print(F("-"));

    // Dwell Value Display
    tft.fillRect(cardX2 + 46, cardY + 26, 56, 32, COLOR_BG);
    tft.drawRect(cardX2 + 46, cardY + 26, 56, 32, COLOR_BORDER);
    tft.setTextColor(COLOR_GREEN, COLOR_BG);
    tft.setTextSize(2);
    tft.setCursor(cardX2 + 50, cardY + 34);
    if (dwellTime < 10) tft.print(F(" "));
    tft.print(dwellTime);
    tft.setTextSize(1);
    tft.print(F("m"));

    // Increment Button [+]
    tft.fillRoundRect(cardX2 + 106, cardY + 26, 34, 32, 4, COLOR_HEADER);
    tft.drawRoundRect(cardX2 + 106, cardY + 26, 34, 32, 4, COLOR_ACCENT);
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(cardX2 + 116, cardY + 34);
    tft.print(F("+"));

    // --- 3. POWER STEPPER ---
    int cardX3 = 322;
    tft.fillRoundRect(cardX3, cardY, cardW, cardH, 6, COLOR_CARD);
    tft.drawRoundRect(cardX3, cardY, cardW, cardH, 6, COLOR_BORDER);
    tft.setTextColor(COLOR_ACCENT, COLOR_CARD);
    tft.setTextSize(1);
    tft.setCursor(cardX3 + 10, cardY + 8);
    tft.print(F("TX POWER (PA)"));

    // Decrement Button [-]
    tft.fillRoundRect(cardX3 + 8, cardY + 26, 34, 32, 4, COLOR_HEADER);
    tft.drawRoundRect(cardX3 + 8, cardY + 26, 34, 32, 4, COLOR_ACCENT);
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(cardX3 + 18, cardY + 34);
    tft.print(F("-"));

    // Power Value Display
    tft.fillRect(cardX3 + 46, cardY + 26, 56, 32, COLOR_BG);
    tft.drawRect(cardX3 + 46, cardY + 26, 56, 32, COLOR_BORDER);
    tft.setTextColor(COLOR_RED, COLOR_BG);
    tft.setTextSize(2);
    tft.setCursor(cardX3 + 52, cardY + 34);
    tft.print(PA_NAMES[paIndex]);

    // Increment Button [+]
    tft.fillRoundRect(cardX3 + 106, cardY + 26, 34, 32, 4, COLOR_HEADER);
    tft.drawRoundRect(cardX3 + 106, cardY + 26, 34, 32, 4, COLOR_ACCENT);
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(2);
    tft.setCursor(cardX3 + 116, cardY + 34);
    tft.print(F("+"));
}

void drawPrimaryControls() {
    int ctrlY = 196;
    int ctrlH = 46;

    // START / STOP Button (Left)
    int btnW = 224;
    uint16_t startBg = jamming ? COLOR_RED : COLOR_GREEN;
    uint16_t startText = jamming ? COLOR_TEXT : COLOR_DARK;

    tft.fillRoundRect(10, ctrlY, btnW, ctrlH, 8, startBg);
    tft.drawRoundRect(10, ctrlY, btnW, ctrlH, 8, COLOR_TEXT);
    tft.setTextColor(startText, startBg);
    tft.setTextSize(2);
    tft.setCursor(jamming ? 42 : 36, ctrlY + 15);
    tft.print(jamming ? F("[ STOP JAMMER ]") : F("[ START JAMMER ]"));

    // PAUSE / RESUME Button (Right)
    uint16_t pauseBg = paused ? COLOR_ACCENT : COLOR_AMBER;
    uint16_t pauseText = COLOR_DARK;

    tft.fillRoundRect(246, ctrlY, btnW, ctrlH, 8, pauseBg);
    tft.drawRoundRect(246, ctrlY, btnW, ctrlH, 8, COLOR_TEXT);
    tft.setTextColor(pauseText, pauseBg);
    tft.setTextSize(2);
    tft.setCursor(paused ? 276 : 282, ctrlY + 15);
    tft.print(paused ? F("[ RESUME RF ]") : F("[ PAUSE RF ]"));
}

void drawSpectrumBar() {
    int barX = 10;
    int barY = 286;
    int barW = 460;
    int barH = 24;

    tft.fillRect(barX, barY, barW, barH, COLOR_DARK);
    tft.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, COLOR_BORDER);

    // Draw 4 Anchor Channel Markers (10, 42, 74, 106)
    for (int i = 0; i < NUM_ANCHOR_CHANNELS; i++) {
        int anchX = barX + map(ANCHOR_CHANNELS[i], CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS, 0, barW);
        tft.drawFastVLine(anchX, barY, barH, COLOR_ACCENT);
        tft.drawFastVLine(anchX + 1, barY, barH, COLOR_ACCENT);
    }

    // Draw Active Channel Cursor
    int curX = barX + map(currentChannel, CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS, 0, barW);
    tft.fillRect(curX - 2, barY + 2, 5, barH - 4, jamming ? COLOR_RED : COLOR_AMBER);
}

void updateTelemetryDisplay() {
    int infoY = 250;
    tft.fillRect(10, infoY, 460, 28, COLOR_HEADER);
    tft.drawRect(10, infoY, 460, 28, COLOR_BORDER);

    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setTextSize(1);

    // Frequency
    tft.setCursor(20, infoY + 10);
    tft.print(F("FREQ: "));
    tft.setTextColor(COLOR_AMBER, COLOR_HEADER);
    tft.print(2400 + currentChannel);
    tft.print(F(" MHz"));

    // TX Packets
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setCursor(160, infoY + 10);
    tft.print(F("TX TOTAL: "));
    tft.setTextColor(COLOR_GREEN, COLOR_HEADER);
    tft.print(txCount);

    // TX Rate
    tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
    tft.setCursor(320, infoY + 10);
    tft.print(F("RATE: "));
    tft.setTextColor(COLOR_ACCENT, COLOR_HEADER);
    tft.print(currentTxRate);
    tft.print(F(" pkt/s"));

    drawSpectrumBar();
}

void renderFullUI() {
    tft.fillScreen(COLOR_BG);
    drawHeader();
    drawModeButtons();
    drawSteppers();
    drawPrimaryControls();
    updateTelemetryDisplay();
}

// ---------------- Touch Event Processing ----------------
void handleTouch(int tx, int ty) {
    // Mode Buttons Row 1 (y: 44 to 76)
    if (ty >= 44 && ty <= 76) {
        int btnW1 = 110;
        for (int i = 0; i < 4; i++) {
            int x = 10 + i * (btnW1 + 8);
            if (tx >= x && tx <= (x + btnW1)) {
                mode = (JammerMode)i;
                if (mode == JAM_AUTOHOP) simulated_hop = micros() / DWELL_US;
                Serial.print(F("[TOUCH] Selected Mode: "));
                Serial.println(MODE_NAMES[mode]);
                drawModeButtons();
                return;
            }
        }
    }

    // Mode Buttons Row 2 (y: 82 to 114)
    if (ty >= 82 && ty <= 114) {
        int btnW2 = 148;
        for (int i = 0; i < 3; i++) {
            int x = 10 + i * (btnW2 + 9);
            if (tx >= x && tx <= (x + btnW2)) {
                mode = (JammerMode)(i + 4);
                Serial.print(F("[TOUCH] Selected Mode: "));
                Serial.println(MODE_NAMES[mode]);
                drawModeButtons();
                return;
            }
        }
    }

    // Stepper Buttons Row (y: 146 to 178)
    if (ty >= 146 && ty <= 178) {
        // Channel Decrement [-]
        if (tx >= 18 && tx <= 52) {
            stepChannel(-1);
            drawSteppers();
            drawSpectrumBar();
            return;
        }
        // Channel Increment [+]
        if (tx >= 116 && tx <= 150) {
            stepChannel(1);
            drawSteppers();
            drawSpectrumBar();
            return;
        }
        // Dwell Decrement [-]
        if (tx >= 174 && tx <= 208) {
            stepDwell(-1);
            drawSteppers();
            return;
        }
        // Dwell Increment [+]
        if (tx >= 272 && tx <= 306) {
            stepDwell(1);
            drawSteppers();
            return;
        }
        // Power Decrement [-]
        if (tx >= 330 && tx <= 364) {
            stepPower(-1);
            drawSteppers();
            return;
        }
        // Power Increment [+]
        if (tx >= 428 && tx <= 462) {
            stepPower(1);
            drawSteppers();
            return;
        }
    }

    // Primary Control Buttons (y: 196 to 242)
    if (ty >= 196 && ty <= 242) {
        // START / STOP (Left)
        if (tx >= 10 && tx <= 234) {
            if (jamming) stopJammer();
            else startJammer();
            drawHeader();
            drawPrimaryControls();
            return;
        }
        // PAUSE / RESUME (Right)
        if (tx >= 246 && tx <= 470) {
            togglePause();
            drawHeader();
            drawPrimaryControls();
            return;
        }
    }

    // Direct Spectrum Tap (y: 280 to 316)
    if (ty >= 280 && ty <= 316) {
        if (tx >= 10 && tx <= 470) {
            int selectedCh = map(tx, 10, 470, CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS);
            setChannel(selectedCh);
            mode = JAM_SPOT;
            drawModeButtons();
            drawSteppers();
            drawSpectrumBar();
            return;
        }
    }
}

// ---------------- Serial Command Processor ----------------
void processSerial() {
    while (Serial.available()) {
        char cmd = Serial.read();
        switch (cmd) {
            case 'j':
            case 'J':
                if (jamming) stopJammer();
                else startJammer();
                drawHeader();
                drawPrimaryControls();
                break;

            case 'k':
            case 'K':
            case ' ':
                togglePause();
                drawHeader();
                drawPrimaryControls();
                break;

            case '+':
            case '=':
                stepChannel(1);
                drawSteppers();
                drawSpectrumBar();
                break;

            case '-':
            case '_':
                stepChannel(-1);
                drawSteppers();
                drawSpectrumBar();
                break;

            case '[':
                stepDwell(-1);
                drawSteppers();
                break;

            case ']':
                stepDwell(1);
                drawSteppers();
                break;

            case '<':
            case ',':
                stepPower(-1);
                drawSteppers();
                break;

            case '>':
            case '.':
                stepPower(1);
                drawSteppers();
                break;

            case '1':
            case 's':
            case 'S':
                mode = JAM_SPOT;
                drawModeButtons();
                break;

            case '2':
            case 'b':
            case 'B':
                mode = JAM_SWEEP;
                drawModeButtons();
                break;

            case '3':
            case 'r':
            case 'R':
                mode = JAM_RANDOM;
                drawModeButtons();
                break;

            case '4':
            case 'h':
            case 'H':
                mode = JAM_AUTOHOP;
                simulated_hop = micros() / DWELL_US;
                drawModeButtons();
                break;

            case '5':
            case 'a':
            case 'A':
                mode = JAM_ADAPTIVE;
                drawModeButtons();
                break;

            case '6':
            case 'u':
            case 'U':
                mode = JAM_BURST;
                drawModeButtons();
                break;

            case '7':
            case 'x':
            case 'X':
                mode = JAM_ANCHOR;
                drawModeButtons();
                break;

            case 'c':
            case 'C': {
                int ch = Serial.parseInt();
                if (ch >= CHANNEL_BASE && ch < CHANNEL_BASE + NUM_CHANNELS) {
                    setChannel(ch);
                    mode = JAM_SPOT;
                    drawModeButtons();
                    drawSteppers();
                    drawSpectrumBar();
                }
                break;
            }

            case 'd':
            case 'D': {
                int ms = Serial.parseInt();
                if (ms > 0) {
                    dwellTime = ms;
                    drawSteppers();
                }
                break;
            }

            case 'p':
            case 'P': {
                int p = Serial.parseInt();
                if (p >= 0 && p <= 3) {
                    paIndex = p;
                    radio.setPALevel(PA_VALS[paIndex]);
                    drawSteppers();
                }
                break;
            }

            case '?':
                Serial.println(F("\n--- HopperNet Jammer CLI ---"));
                Serial.println(F("j: Start/Stop | k/space: Pause/Resume"));
                Serial.println(F("+/-: Channel Step | [/]: Dwell Step | </>: Power Step"));
                Serial.println(F("1: Spot | 2: Sweep | 3: Random | 4: AutoHop | 5: Adaptive | 6: Burst | 7: Anchor"));
                Serial.println(F("c <ch>: Set Ch | d <ms>: Set Dwell | p <0-3>: Set PA\n"));
                break;
        }
    }
}

// ---------------- Setup ----------------
void setup() {
    Serial.begin(BAUD);
    delay(500);
    randomSeed(analogRead(0));

    Serial.println(F("\n=============================================="));
    Serial.println(F("  HopperNet RF Jammer & Adversary Console     "));
    Serial.println(F("  Arduino Mega 2560 + 3.5\" TFT Touch + RF24   "));
    Serial.println(F("=============================================="));

    // Initialize Touchscreen Display
    tft.reset();
    screenID = tft.readID();
    if (screenID == 0xD3D3 || screenID == 0x0 || screenID == 0xFFFF) {
        screenID = 0x9486;
    }
    tft.begin(screenID);
    tft.setRotation(1); // Landscape (480x320)

    // Initialize nRF24L01+ Radio
    if (!radio.begin()) {
        Serial.println(F("[JAMMER] RF24 init FAILED — Check wiring (CE=43, CSN=45, SPI 50-52)"));
    } else {
        Serial.println(F("[JAMMER] RF24 Initialized Successfully."));
        radio.setPALevel(PA_VALS[paIndex]);
        radio.setDataRate(RF24_250KBPS);
        radio.setPayloadSize(32);
        radio.setAutoAck(false);
        radio.setChannel(currentChannel);
        radio.startListening();
    }

    renderFullUI();
    restoreLcdPins();
}

// ---------------- Main Loop ----------------
void loop() {
    static unsigned long lastDwellAction = 0;
    unsigned long now = millis();

    // 1. Attack Strategy Execution
    if (jamming && !paused) {
        switch (mode) {
            case JAM_SPOT:
                if (now - lastDwellAction >= (unsigned long)dwellTime) {
                    radio.setChannel(currentChannel);
                    blastPackets(4);
                    lastDwellAction = now;
                }
                break;

            case JAM_SWEEP:
                sweepCh++;
                if (sweepCh >= CHANNEL_BASE + NUM_CHANNELS) sweepCh = CHANNEL_BASE;
                currentChannel = sweepCh;
                radio.setChannel(currentChannel);
                blastPackets(2);
                break;

            case JAM_RANDOM:
                if (now - lastDwellAction >= (unsigned long)dwellTime) {
                    currentChannel = random(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS);
                    radio.setChannel(currentChannel);
                    blastPackets(3);
                    lastDwellAction = now;
                }
                break;

            case JAM_AUTOHOP:
                if (now - lastDwellAction >= 25) { // Track 25ms FHSS dwell
                    simulated_hop++;
                    currentChannel = channel_for_hop(simulated_hop, FHSS_SEED, dummy_empty_blacklist);
                    radio.setChannel(currentChannel);
                    blastPackets(3);
                    lastDwellAction = now;
                }
                break;

            case JAM_ADAPTIVE:
                // Quick energy sniff across 4 channels then blast hot channel
                radio.startListening();
                for (int attempt = 0; attempt < 4; attempt++) {
                    int testCh = random(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS);
                    radio.setChannel(testCh);
                    delayMicroseconds(130);
                    if (radio.testRPD()) {
                        currentChannel = testCh;
                        break;
                    }
                }
                radio.stopListening();
                radio.setChannel(currentChannel);
                blastPackets(4);
                break;

            case JAM_BURST:
                // High-density pulse burst
                radio.setChannel(currentChannel);
                blastPackets(8);
                delayMicroseconds(300);
                break;

            case JAM_ANCHOR:
                if (now - lastDwellAction >= (unsigned long)dwellTime) {
                    anchorIdx = (anchorIdx + 1) % NUM_ANCHOR_CHANNELS;
                    currentChannel = ANCHOR_CHANNELS[anchorIdx];
                    radio.setChannel(currentChannel);
                    blastPackets(4);
                    lastDwellAction = now;
                }
                break;
        }
    }

    // 2. Touch Screen Handling
    TSPoint p = ts.getPoint();
    restoreLcdPins();

    if (p.z > MINPRESSURE && p.z < MAXPRESSURE) {
        int pixelX = map(p.x, TS_MINX, TS_MAXX, 0, tft.width());
        int pixelY = map(p.y, TS_MINY, TS_MAXY, 0, tft.height());
        pixelX = constrain(pixelX, 0, tft.width() - 1);
        pixelY = constrain(pixelY, 0, tft.height() - 1);

        if (!touchDebounce && (now - lastTouchTime > 180)) {
            touchDebounce = true;
            lastTouchTime = now;
            handleTouch(pixelX, pixelY);
            restoreLcdPins();
        }
    } else {
        touchDebounce = false;
    }

    // 3. Serial Command Handling
    processSerial();

    // 4. Rate Counter & Telemetry Refresh (Every 500ms)
    if (now - lastTxRateCheck >= 500) {
        unsigned long diff = txCount - txCountSnapshot;
        currentTxRate = (unsigned int)(diff * 1000UL / (now - lastTxRateCheck));
        txCountSnapshot = txCount;
        lastTxRateCheck = now;

        updateTelemetryDisplay();
        restoreLcdPins();
    }
}