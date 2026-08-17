// HopperNet Mega Touchscreen Diagnostic Tool (Calibrated for ILI9486 Shield)
// Verified Pinout: YP = A2, XM = A3, YM = 8, XP = 9

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>

#define BAUD 115200

MCUFRIEND_kbv tft;

// ----------------- Calibrated Pin Configuration -----------------
#define YP A2  // Must be an analog pin
#define XM A3  // Must be an analog pin
#define YM 8   // Digital pin
#define XP 9   // Digital pin

// Resistive Touch Calibration Constants
// For ILI9486 3.5" (320x480 resolution)
#define TS_MINX 100
#define TS_MAXX 920
#define TS_MINY 120
#define TS_MAXY 940

// Valid touch pressure thresholds
#define MINPRESSURE 10
#define MAXPRESSURE 1000

TouchScreen ts = TouchScreen(XP, YP, XM, YM, 300);

// Color Palette (16-bit 565)
#define BLACK       0x0000
#define NAVY        0x000F
#define DARKGREEN   0x03E0
#define DARKCYAN    0x03EF
#define LIGHTGREY   0xC618
#define DARKGREY    0x7BEF
#define BLUE        0x001F
#define GREEN       0x07E0
#define CYAN        0x07FF
#define RED         0xF800
#define MAGENTA     0xF81F
#define YELLOW      0xFFE0
#define WHITE       0xFFFF
#define ORANGE      0xFD20
#define CARD_BG     0x18E3
#define BTN_BG      0x0277
#define BTN_ACTIVE  0x07E0

int btnX = 130;
int btnY = 185;
int btnW = 220;
int btnH = 65;

bool buttonState = false;
unsigned long lastTouchTime = 0;
uint16_t screenID = 0;

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

void drawHeader() {
    tft.fillRect(0, 0, tft.width(), 42, CARD_BG);
    tft.drawFastHLine(0, 42, tft.width(), CYAN);

    tft.setTextColor(WHITE, CARD_BG);
    tft.setTextSize(2);
    tft.setCursor(15, 12);
    tft.print(F("HopperNet Touch Verified"));

    // Chip ID
    tft.fillRect(tft.width() - 120, 7, 110, 28, DARKCYAN);
    tft.drawRect(tft.width() - 120, 7, 110, 28, WHITE);
    tft.setTextColor(YELLOW, DARKCYAN);
    tft.setTextSize(1);
    tft.setCursor(tft.width() - 114, 12);
    tft.print(F("ILI9486: 0x"));
    tft.print(screenID, HEX);
    tft.setCursor(tft.width() - 114, 22);
    tft.print(tft.width());
    tft.print(F("x"));
    tft.print(tft.height());
}

void drawSmiley(int centerX, int centerY, int radius, bool happy) {
    uint16_t faceColor = happy ? YELLOW : LIGHTGREY;
    tft.fillCircle(centerX, centerY, radius, faceColor);
    tft.drawCircle(centerX, centerY, radius, BLACK);
    tft.drawCircle(centerX, centerY, radius - 1, BLACK);

    int eyeOffsetX = radius / 3;
    int eyeOffsetY = radius / 3;
    int eyeRadius = max(3, radius / 8);

    // Eyes
    tft.fillCircle(centerX - eyeOffsetX, centerY - eyeOffsetY, eyeRadius, BLACK);
    tft.fillCircle(centerX + eyeOffsetX, centerY - eyeOffsetY, eyeRadius, BLACK);

    // Highlights
    tft.fillCircle(centerX - eyeOffsetX + 1, centerY - eyeOffsetY - 1, max(1, eyeRadius / 2), WHITE);
    tft.fillCircle(centerX + eyeOffsetX + 1, centerY - eyeOffsetY - 1, max(1, eyeRadius / 2), WHITE);

    if (happy) {
        // Cheeks
        tft.fillCircle(centerX - eyeOffsetX - 5, centerY + 2, max(2, radius / 8), ORANGE);
        tft.fillCircle(centerX + eyeOffsetX + 5, centerY + 2, max(2, radius / 8), ORANGE);

        // Smile
        int mouthRadius = radius / 2;
        for (int a = 20; a <= 160; a += 4) {
            float rad = a * 0.0174533;
            int mx = centerX + (int)(mouthRadius * cos(rad));
            int my = centerY + (int)(mouthRadius * sin(rad)) + 2;
            tft.fillCircle(mx, my, 2, BLACK);
        }
    } else {
        // Neutral mouth line
        int mouthRadius = radius / 2;
        tft.drawFastHLine(centerX - mouthRadius + 4, centerY + mouthRadius / 2, mouthRadius * 2 - 8, BLACK);
        tft.drawFastHLine(centerX - mouthRadius + 4, centerY + mouthRadius / 2 + 1, mouthRadius * 2 - 8, BLACK);
    }
}

void drawButton(bool active) {
    uint16_t bgColor = active ? GREEN : BTN_BG;
    uint16_t borderColor = active ? WHITE : CYAN;
    uint16_t textColor = active ? BLACK : WHITE;

    tft.fillRoundRect(btnX, btnY, btnW, btnH, 12, bgColor);
    tft.drawRoundRect(btnX, btnY, btnW, btnH, 12, borderColor);
    tft.drawRoundRect(btnX + 1, btnY + 1, btnW - 2, btnH - 2, 11, borderColor);

    tft.setTextColor(textColor, bgColor);
    tft.setTextSize(2);
    if (active) {
        tft.setCursor(btnX + 22, btnY + 24);
        tft.print(F("TOUCHED! :-) OK"));
    } else {
        tft.setCursor(btnX + 28, btnY + 24);
        tft.print(F("PRESS BUTTON"));
    }
}

void updateStatusBox(const char* statusText, uint16_t color, int rawX, int rawY, int rawZ, int pixelX, int pixelY) {
    tft.fillRect(10, 260, tft.width() - 20, 52, CARD_BG);
    tft.drawRect(10, 260, tft.width() - 20, 52, color);

    tft.setTextColor(color, CARD_BG);
    tft.setTextSize(1);
    tft.setCursor(20, 267);
    tft.print(F("STATUS: "));
    tft.print(statusText);

    tft.setTextColor(WHITE, CARD_BG);
    tft.setCursor(20, 281);
    tft.print(F("RAW ADC: X="));
    tft.print(rawX);
    tft.print(F(" Y="));
    tft.print(rawY);
    tft.print(F(" Pressure(Z)="));
    tft.print(rawZ);

    tft.setCursor(20, 295);
    tft.print(F("PIXEL: ("));
    tft.print(pixelX);
    tft.print(F(", "));
    tft.print(pixelY);
    tft.print(F(") | Pinout: YP=A2, XM=A3, YM=8, XP=9"));
}

void setupUI() {
    tft.fillScreen(BLACK);
    drawHeader();
    drawSmiley(240, 110, 45, false);

    tft.setTextColor(LIGHTGREY, BLACK);
    tft.setTextSize(1);
    tft.setCursor(150, 168);
    tft.print(F("Touch screen or press button"));

    drawButton(false);
    updateStatusBox("READY - Touch Screen to Test", YELLOW, 0, 0, 0, 0, 0);
}

void setup() {
    Serial.begin(BAUD);
    delay(1000);

    Serial.println(F("\n=============================================="));
    Serial.println(F("  HopperNet Mega Touchscreen Diagnostic Tool  "));
    Serial.println(F("=============================================="));

    tft.reset();
    screenID = tft.readID();
    if (screenID == 0xD3D3 || screenID == 0x0 || screenID == 0xFFFF) {
        screenID = 0x9486;
    }

    tft.begin(screenID);
    tft.setRotation(1); // Landscape (480x320)

    btnX = (tft.width() - btnW) / 2;

    Serial.print(F("[TFT] Detected LCD Driver Chipset ID: 0x"));
    Serial.println(screenID, HEX);
    Serial.print(F("[TFT] Resolution: "));
    Serial.print(tft.width());
    Serial.print(F(" x "));
    Serial.println(tft.height());
    Serial.println(F("[TOUCH] Active Pinout: YP=A2, XM=A3, YM=8, XP=9"));
    Serial.println(F("==============================================\n"));

    setupUI();
    restoreLcdPins();
}

void loop() {
    // 1. Query touch coordinates
    TSPoint p = ts.getPoint();

    // 2. IMMEDIATELY restore LCD pin modes
    restoreLcdPins();

    // 3. Check if pressure threshold is met
    if (p.z > MINPRESSURE && p.z < MAXPRESSURE) {
        // Map raw touch coordinates to landscape orientation
        int pixelX = map(p.x, TS_MINX, TS_MAXX, 0, tft.width());
        int pixelY = map(p.y, TS_MINY, TS_MAXY, 0, tft.height());

        pixelX = constrain(pixelX, 0, tft.width() - 1);
        pixelY = constrain(pixelY, 0, tft.height() - 1);

        lastTouchTime = millis();

        Serial.print(F("[TOUCH DETECTED] Raw: (X="));
        Serial.print(p.x);
        Serial.print(F(", Y="));
        Serial.print(p.y);
        Serial.print(F(", Z="));
        Serial.print(p.z);
        Serial.print(F(") -> Pixel: ("));
        Serial.print(pixelX);
        Serial.print(F(", "));
        Serial.print(pixelY);
        Serial.print(F(")"));

        // Check if inside Button area
        bool inButton = (pixelX >= btnX && pixelX <= (btnX + btnW) &&
                         pixelY >= btnY && pixelY <= (btnY + btnH));

        if (inButton) {
            Serial.println(F(" [BUTTON PRESSED! :-)]"));
            if (!buttonState) {
                buttonState = true;
                drawButton(true);
                drawSmiley(240, 110, 45, true); // Happy Smiley
                updateStatusBox("BUTTON PRESSED! SMILEY ACTIVE :-)", GREEN, p.x, p.y, p.z, pixelX, pixelY);
                restoreLcdPins();
            }
        } else {
            Serial.println(F(" [CANVAS TAP]"));
            tft.fillCircle(pixelX, pixelY, 3, MAGENTA);
            tft.drawCircle(pixelX, pixelY, 5, YELLOW);
            updateStatusBox("SCREEN TOUCH DETECTED", CYAN, p.x, p.y, p.z, pixelX, pixelY);
            restoreLcdPins();
        }
    } else {
        // If button was pressed and released for > 400ms, reset to idle
        if (buttonState && (millis() - lastTouchTime > 400)) {
            buttonState = false;
            drawButton(false);
            drawSmiley(240, 110, 45, false); // Return to neutral
            updateStatusBox("TOUCH RELEASED - Ready", YELLOW, 0, 0, 0, 0, 0);
            restoreLcdPins();
            Serial.println(F("[TOUCH EVENT] Released"));
        }
    }

    delay(20);
}
