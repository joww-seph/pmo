/*
 * sketch_pmo.ino — PMO Power Monitor Operator (main ESP32 firmware)
 *
 * ARCHITECTURE OVERVIEW
 * ---------------------
 * The firmware uses FreeRTOS dual-core task scheduling:
 *
 *   Core 1 (loop / Arduino main task)
 *     - Reads PZEM-004T sensor every 2 s
 *     - Runs relay protection evaluation after each sensor read
 *     - Updates TFT face animation, OLED status, and 16x2 LCD
 *     - Handles touchscreen and physical button input
 *     - Reads occupancy status from ESP32-CAM over UART
 *     - Schedules AI TTS pre-fetch at 6 AM and 6 PM
 *
 *   Core 0 (httpTask — FreeRTOS pinned task)
 *     - Posts sensor payload to insert.php every 3 s
 *     - Polls relay.php for remote ON/OFF commands every 3 s
 *     - Handles AES-128-CBC anti-bot cookie refresh
 *     - Runs TTS download prefetch task when triggered
 *
 * Data shared between cores is protected by FreeRTOS mutexes:
 *   payloadMutex — guards the HttpPayload struct
 *   audioMutex   — guards I2S DAC access
 *   sdMutex      — guards SD card SPI bus access
 *
 * DISPLAY HIERARCHY
 *   ILI9488 480x320 TFT  — primary animated face (BMO character)
 *   SSD1306 128x64 OLED  — real-time voltage/current/relay status
 *   16x2 I2C LCD         — rotating 8-screen metric display
 */

#include "FS.h"
#include <SPIFFS.h>
#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <LiquidCrystal_I2C.h>
#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/aes.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <AudioGeneratorWAV.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioOutputI2S.h>
#include <AudioGeneratorMP3.h>
#include <TJpg_Decoder.h>

// ── Hardware pin assignments ──────────────────────────────────────
#define RELAY_PIN   32  // Normally-HIGH output; driven LOW to cut power
#define SDA_PIN     21
#define SCL_PIN     22
#define PZEM_RX     16  // UART2 RX — receives data from PZEM-004T TX
#define PZEM_TX     17  // UART2 TX — sends requests to PZEM-004T RX
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ESP32-CAM communicates occupancy results over UART1 at 9600 baud.
// OCCUPANCY_TIMEOUT: if no person is detected for 60 s, relay trips to save energy.
#define CAM_SERIAL        Serial1
#define CAM_BAUD          9600
#define CAM_RX_PIN        36
#define OCCUPANCY_TIMEOUT 60000UL

// Physical push buttons — active LOW (pulled HIGH internally)
#define BTN_WIFI    39  // Re-open WiFi manager
#define BTN_AI      34  // Play latest AI recommendation audio
#define BTN_LCD     35  // Pause/resume LCD screen rotation

static bool      lcdPaused      = false;
static uint32_t  lastBtnWifi    = 0;
static uint32_t  lastBtnAI      = 0;
static uint32_t  lastBtnLcd     = 0;
constexpr uint32_t BTN_DEBOUNCE = 300;

// ── I2S audio DAC and SD card ─────────────────────────────────────
#define I2S_BCLK      26  // Bit clock
#define I2S_LRC       27  // Left/right clock (word select)
#define I2S_DOUT      5   // Serial data out to DAC
#define SD_CS         25  // SD card chip-select on HSPI bus
#define ALERT_FOLDER  "/alerts"

static AudioOutputI2S    *i2sOut      = nullptr;
static AudioFileSource   *audioSrc    = nullptr;
static AudioGeneratorWAV *wavGen      = nullptr;
static volatile bool      audioPlaying    = false;
static TaskHandle_t       audioTaskHandle = nullptr;
static SemaphoreHandle_t  audioMutex      = nullptr;
static SemaphoreHandle_t  sdMutex         = nullptr;
static volatile bool      prefetchRunning = false;

static SPIClass sdSPI(HSPI);

#define TTS_GEN_URL   "https://pmo.infinityfree.me/generate_tts.php"
#define TTS_INFO_URL  "https://pmo.infinityfree.me/get_tts_info.php"
#define TTS_DL_URL    "https://pmo.infinityfree.me/download_tts.php"

#define SERVER_URL     "https://pmo.infinityfree.me/insert.php"
#define RELAY_POLL_URL "https://pmo.infinityfree.me/relay.php"
#define DEVICE_ID   "PMO-ESP32-001"
constexpr uint32_t SERVER_INTERVAL = 3000;
uint32_t lastServer = 0;

struct HttpPayload {
  float voltage, current, power, energy, frequency, pf;
  float apparentPower, reactivePower;
  float costPerHour, costPerMonth;
  float wastedPower, safetyMargin, co2Kg;
  char  loadType[16];
  char  wifiSsid[33];
  int   relayState;
  char  tripReason[16];
};

static HttpPayload    httpPayload;
static SemaphoreHandle_t payloadMutex   = nullptr;
static volatile bool  httpSendPending   = false;
static volatile bool  httpPollPending   = false;

static String    cachedCookie    = "";
static uint32_t  cookieFetchedAt = 0;
constexpr uint32_t COOKIE_TTL   = 6UL * 60UL * 60UL * 1000UL;
// ── Philippine grid and circuit constants ─────────────────────────
constexpr float MAX_CIRCUIT_A    = 16.0f;    // Standard 16 A household breaker rating
constexpr float NOMINAL_V        = 220.0f;   // PH nominal supply voltage
constexpr float NOMINAL_HZ       = 60.0f;    // PH grid frequency
constexpr float COST_PER_KWH     = 13.8161f; // Philippine electricity rate (₱/kWh)
constexpr float CO2_PER_KWH      = 0.6032f;  // PH grid emission factor (kg CO₂/kWh)

// ── Relay protection thresholds (hysteresis pairs) ────────────────
// Each fault has a TRIP threshold that opens the relay and a RESTORE threshold
// that is further inside the safe zone to prevent chatter near the boundary.
constexpr float RELAY_UV_TRIP    = 195.0f;   // Undervoltage trip  < 195 V
constexpr float RELAY_OV_TRIP    = 250.0f;   // Overvoltage trip   > 250 V
constexpr float RELAY_UV_RESTORE = 205.0f;   // Undervoltage restore ≥ 205 V
constexpr float RELAY_OV_RESTORE = 240.0f;   // Overvoltage restore  ≤ 240 V
constexpr float RELAY_OC_TRIP    = 15.0f;    // Overcurrent trip   > 15 A (93% of breaker)
constexpr float RELAY_OC_RESTORE = 13.5f;    // Overcurrent restore ≤ 13.5 A
constexpr float RELAY_OP_TRIP    = 3200.0f;  // Overpower trip     > 3200 W
constexpr float RELAY_OP_RESTORE = 3000.0f;  // Overpower restore  ≤ 3000 W
constexpr float RELAY_OF_TRIP    = 63.0f;    // Over-frequency trip  > 63 Hz
constexpr float RELAY_UF_TRIP    = 57.0f;    // Under-frequency trip < 57 Hz
constexpr float RELAY_OF_RESTORE = 61.5f;    // Over-frequency restore  ≤ 61.5 Hz
constexpr float RELAY_UF_RESTORE = 58.5f;    // Under-frequency restore ≥ 58.5 Hz
// Low-PF trip is only active when P > 100 W to ignore sensor noise at near-zero load
constexpr float RELAY_LPF_TRIP   = 0.50f;    // Low power factor trip    < 0.50
constexpr float RELAY_LPF_RESTORE= 0.65f;    // Low power factor restore ≥ 0.65

// Minimum time the relay stays open after a trip before auto-restore is attempted
constexpr uint32_t RELAY_COOLDOWN= 5000;     // ms

constexpr uint32_t PZEM_INTERVAL = 2000;
constexpr uint32_t LCD_INTERVAL  = 3000;

#define CALIBRATION_FILE  "/TouchCalData2"
#define WIFI_CRED_FILE    "/wifi_creds.json"
#define REPEAT_CAL         false
#define WIFI_TIMEOUT_MS    9000

#define SW  480
#define SH  320

#define CLR_BG      0x8DB3   
#define CLR_PANEL   0x6CF0   
#define CLR_HDR     0x3D4B   
#define CLR_TEXT    0x10C2   
#define CLR_FRAME   0xC6F8   
#define CLR_SUB     0x5BCC   
#define CLR_ACCENT  0x4C8D   
#define CLR_YELLOW  0xD544   
#define CLR_RED     0xB904   
#define CLR_INPUT   0xB677   
#define CLR_INVTEXT 0xE79C   

#define HDR_H        36
#define MARGIN        8
#define NET_Y        (HDR_H + 6)
#define NET_X        MARGIN
#define NET_W        (SW - MARGIN * 2)
#define NET_IH       34
#define NET_MAX       7
#define PW_BOX_X     MARGIN
#define PW_BOX_Y     (HDR_H + 6)
#define PW_BOX_W     (SW - MARGIN * 2)
#define PW_BOX_H     34
#define KBD_COLS     10
#define KBD_ROWS      4
#define KBD_KW       43
#define KBD_KH       34
#define KBD_GAP       2
#define KBD_X        MARGIN
#define KBD_Y        (HDR_H + 6 + PW_BOX_H + 14)
#define SPL_Y        (KBD_Y + KBD_ROWS * (KBD_KH + KBD_GAP) + 6)

bool pmoMode = false;

bool     kbCursorVisible = true;
uint32_t lastCursorBlink = 0;
constexpr uint32_t CURSOR_BLINK_MS = 500;

TFT_eSPI          tft;
LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial    pzemSerial(2);
PZEM004Tv30       pzem(pzemSerial, PZEM_RX, PZEM_TX);

byte pesoChar[8] = {
  B11110, B10001, B11111, B10001,
  B11110, B10000, B10000, B10000
};

enum AppState { S_BOOT, S_SCAN, S_LIST, S_KEYBOARD, S_CONNECTING, S_DONE };
AppState wifiState = S_BOOT;

struct Net { String ssid; int32_t rssi; bool secured; };
Net  nets[20];
int  netCount   = 0;
int  listOffset = 0;
int  selNet     = -1;

char pwBuf[65]  = "";
int  pwLen      = 0;
bool shifted    = false;

TFT_eSPI_Button kbBtn[KBD_ROWS][KBD_COLS];
TFT_eSPI_Button btnShift, btnSpace, btnDel, btnOK, btnRescan;

const char KL[KBD_ROWS][KBD_COLS] = {
  {'1','2','3','4','5','6','7','8','9','0'},
  {'q','w','e','r','t','y','u','i','o','p'},
  {'a','s','d','f','g','h','j','k','l',';'},
  {'z','x','c','v','b','n','m','.','-','_'}
};
const char KU[KBD_ROWS][KBD_COLS] = {
  {'!','@','#','$','%','^','&','*','(',')'},
  {'Q','W','E','R','T','Y','U','I','O','P'},
  {'A','S','D','F','G','H','J','K','L',':'},
  {'Z','X','C','V','B','N','M','>','<','?'}
};

enum FaceState {
  FACE_IDLE_HUMAN, FACE_IDLE_NOHUMAN,
  FACE_RELAY_TRIP, FACE_RELAY_RESTORED,
  FACE_TALKING, FACE_TOUCH
};
FaceState currentFace     = FACE_IDLE_NOHUMAN;
uint32_t  faceTimerMs     = 0;
bool      isBlink         = false;
uint32_t  blinkStartMs    = 0;
uint32_t  lastBlinkMs     = 0;
int       talkFrame       = 1;
uint32_t  lastTalkMs      = 0;
int       relayTripFrame  = 1;
uint32_t  lastRelayTripMs = 0;

// ── Sensor metrics ────────────────────────────────────────────────
// Raw readings come from the PZEM-004T. All other fields are derived
// inside calculateMetrics() from those six raw values.
struct Metrics {
  float voltage, current, power, energy, frequency, pf;
  float apparentPower, reactivePower;        // Power triangle (S, Q)
  float costPerHour, costPerDay, costPerMonth; // Cost projections at PH rate
  float wastedPower, safetyMargin;           // Reactive waste; headroom to breaker
  float voltageVariability, frequencyDeviation; // Grid quality indicators
  float co2Kg;                               // Cumulative CO₂ from energy_kwh
  String loadType;                           // Classified from PF: Resistive/Mixed/Inductive/Reactive
};
Metrics live; // Updated every PZEM_INTERVAL (2 s) on Core 1

// ── Relay fault classification ────────────────────────────────────
// Used to record why the relay tripped so the LCD and cloud log display
// the correct reason even after the fault has cleared.
enum TripReason {
  TRIP_NONE = 0, TRIP_UNDERVOLT, TRIP_OVERVOLT,
  TRIP_OVERCURRENT, TRIP_OVERPOWER, TRIP_FREQUENCY, TRIP_LOWPF,
  TRIP_NO_OCCUPANCY  // Triggered by ESP32-CAM when room is empty for 60 s
};
bool       relayState   = true;   // true = ON (energised), false = OFF (tripped)
bool       relayTripped = false;  // Distinguishes a protection trip from a manual OFF
TripReason tripReason   = TRIP_NONE;
uint32_t   lastTripMs   = 0;     // Timestamp of last trip (used for cooldown guard)

static uint32_t  lastPersonSeen   = 0;
static bool      occupancyTripped = false;
static bool      occupancyActive  = false;
static bool      personPresent    = false;

int lcdScreen = 0;
constexpr int LCD_SCREENS = 8;

uint32_t lastPzem = 0;
uint32_t lastLcd  = 0;

void touch_calibrate();
bool loadCreds(String &s, String &p);
void saveCreds(const String &s, const String &p);
bool doConnect(const String &ssid, const String &pass);
void doScan();
void showList();
void showKeyboard();
void showConnecting();
void showDone();
void showBoot(const char* msg);
void handleListTouch(uint16_t tx, uint16_t ty);
void handleKbdTouch(uint16_t tx, uint16_t ty);
void redrawPwBox();
void drawHdr(const char* t, bool showBack = false);

void initPMO();
void drawSdJpeg(const char* path);
void updateFace(bool occupied);
Metrics calculateMetrics(float v, float i, float p,
                          float e, float f, float pf);
void evaluateProtection(const Metrics& m);
void updateLCD(const Metrics& m);
void serialDump(const Metrics& m);
const char* tripReasonStr(TripReason r);
void setRelay(bool on);
void tripRelay(TripReason reason);
bool canRestoreRelay(const Metrics& m);
String fmt(float v, int dec = 2);
void lcdPrint(int row, const char* label, const String& value);

void showAIRecommendations();
void handleButtons();

void testI2SAudio();
void playAudioVariant(const char* base);
void playAlert(TripReason reason);
void playRecommendation();
void prefetchRecommendation();
void stopAudio();
void loopAudio();

void handleOccupancyPresence();

void touch_calibrate() {
  uint16_t calData[5];
  uint8_t  ok = 0;

  if (!SPIFFS.begin()) { SPIFFS.format(); SPIFFS.begin(); }

  if (SPIFFS.exists(CALIBRATION_FILE)) {
    if (REPEAT_CAL) {
      SPIFFS.remove(CALIBRATION_FILE);
    } else {
      File f = SPIFFS.open(CALIBRATION_FILE, "r");
      if (f) { if (f.readBytes((char*)calData, 14) == 14) ok = 1; f.close(); }
    }
  }

  if (ok && !REPEAT_CAL) {
    tft.setTouch(calData);
  } else {
    tft.fillScreen(CLR_BG);
    tft.setCursor(20, 0);
    tft.setTextFont(2);
    tft.setTextColor(CLR_TEXT, CLR_BG);
    tft.println("Touch corners as indicated");
    tft.setTextFont(1);
    if (REPEAT_CAL) {
      tft.setTextColor(CLR_RED, CLR_BG);
      tft.println("Set REPEAT_CAL false to stop.");
    }
    tft.calibrateTouch(calData, CLR_HDR, CLR_BG, 15);
    tft.setTextColor(CLR_ACCENT, CLR_BG);
    tft.println("Calibration complete!");
    File f = SPIFFS.open(CALIBRATION_FILE, "w");
    if (f) { f.write((const unsigned char*)calData, 14); f.close(); }
  }
}

void drawHdr(const char* t, bool showBack) {
  tft.fillRect(0, 0, SW, HDR_H, CLR_HDR);
  tft.drawLine(0, 0,     SW, 0,     CLR_FRAME);
  tft.drawLine(0, HDR_H, SW, HDR_H, CLR_TEXT);

  if (showBack) {
    tft.setTextColor(CLR_INVTEXT, CLR_HDR);
    tft.setTextFont(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("< BACK", 6, HDR_H / 2);
    tft.setTextColor(CLR_INVTEXT, CLR_HDR);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(t, SW / 2 + 30, HDR_H / 2);
  } else {
    tft.setTextColor(CLR_INVTEXT, CLR_HDR);
    tft.setTextFont(2);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(t, 8, HDR_H / 2);
  }
}

void showBoot(const char* msg) {
  tft.fillScreen(CLR_BG);
  drawHdr("PMO  WiFi Setup");
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString(msg, SW / 2, SH / 2);
}

void showConnecting() {
  tft.fillScreen(CLR_BG);
  drawHdr("CONNECTING...");
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString(nets[selNet >= 0 ? selNet : 0].ssid, SW / 2, SH / 2 - 10);
  tft.setTextColor(CLR_SUB, CLR_BG);
  tft.setTextFont(1);
  tft.drawString("Please wait...", SW / 2, SH / 2 + 16);
}

void showDone() {
  tft.fillScreen(CLR_BG);
  drawHdr("CONNECTED  —  Starting PMO...");
  tft.setTextColor(CLR_ACCENT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.drawString("OK", SW / 2, SH / 2 - 40);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextFont(2);
  tft.drawString(nets[selNet >= 0 ? selNet : 0].ssid, SW / 2, SH / 2 - 4);
  tft.setTextColor(CLR_SUB, CLR_BG);
  tft.setTextFont(1);
  tft.drawString("IP: " + WiFi.localIP().toString(), SW / 2, SH / 2 + 20);
  tft.drawString("Saved. Launching energy monitor...", SW / 2, SH / 2 + 38);
  delay(1800);  
}

void showList() {
  wifiState = S_LIST;
  tft.fillScreen(CLR_BG);
  drawHdr("SELECT NETWORK");

  tft.setTextFont(1);
  btnRescan.initButton(&tft, SW - 38, HDR_H / 2, 62, 22,
    CLR_FRAME, CLR_ACCENT, CLR_INVTEXT, (char*)"SCAN", 1);
  btnRescan.drawButton();

  for (int i = 0; i < NET_MAX; i++) {
    int idx = i + listOffset;
    if (idx >= netCount) break;
    int ry  = NET_Y + i * (NET_IH + 2);
    bool sel = (idx == selNet);

    uint16_t bg = sel ? CLR_HDR    : CLR_PANEL;
    uint16_t fg = sel ? CLR_INVTEXT : CLR_TEXT;

    tft.fillRect(NET_X, ry, NET_W, NET_IH, bg);
    tft.drawRect(NET_X, ry, NET_W, NET_IH, CLR_TEXT);

    tft.setTextColor(fg, bg);
    tft.setTextFont(2);
    tft.setTextDatum(ML_DATUM);
    String label = nets[idx].ssid;
    if (label.length() > 30) label = label.substring(0, 29) + "~";
    tft.drawString(label, NET_X + 8, ry + NET_IH / 2);

    if (nets[idx].secured) {
      tft.setTextColor(CLR_YELLOW, bg);
      tft.setTextFont(1);
      tft.setTextDatum(MR_DATUM);
      tft.drawString("[P]", NET_X + NET_W - 38, ry + NET_IH / 2);
    }

    int rssi = nets[idx].rssi;
    uint16_t sc = rssi > -60 ? CLR_ACCENT : rssi > -75 ? CLR_YELLOW : CLR_RED;
    const char* sb = rssi > -60 ? "***" : rssi > -75 ? "**" : "*";
    tft.setTextColor(sc, bg);
    tft.setTextFont(1);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(sb, NET_X + NET_W - 6, ry + NET_IH / 2);
  }

  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextFont(1);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("[P]=password required   *=signal strength", MARGIN, SH - 10);

  if (listOffset > 0) {
    tft.setTextColor(CLR_TEXT, CLR_BG);
    tft.setTextDatum(MR_DATUM);
    tft.setTextFont(2);
    tft.drawString("^", SW - 4, NET_Y + 4);
  }
  if (listOffset + NET_MAX < netCount) {
    tft.setTextColor(CLR_TEXT, CLR_BG);
    tft.setTextDatum(MR_DATUM);
    tft.setTextFont(2);
    tft.drawString("v", SW - 4, NET_Y + NET_MAX * (NET_IH + 2) - 18);
  }
}

void handleListTouch(uint16_t tx, uint16_t ty) {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();

  btnRescan.press(btnRescan.contains(tx, ty));
  if (btnRescan.justPressed()) { delay(60); doScan(); return; }

  if (listOffset > 0 && tx > SW - 24 && ty < NET_Y + 20) {
    listOffset--; showList(); return;
  }
  int botY = NET_Y + NET_MAX * (NET_IH + 2);
  if (listOffset + NET_MAX < netCount && tx > SW - 24 && ty > botY - 24) {
    listOffset++; showList(); return;
  }

  for (int i = 0; i < NET_MAX; i++) {
    int idx = i + listOffset;
    if (idx >= netCount) break;
    int ry = NET_Y + i * (NET_IH + 2);
    if (tx >= NET_X && tx <= NET_X + NET_W && ty >= ry && ty <= ry + NET_IH) {
      selNet = idx;
      showList();
      delay(120);
      if (!nets[idx].secured) {
        showConnecting();
        if (doConnect(nets[idx].ssid, "")) {
          saveCreds(nets[idx].ssid, "");
          showDone();
          wifiState = S_DONE;
          pmoMode = true;
          initPMO();
        } else {
          showBoot("Failed. Rescanning..."); delay(1200); doScan();
        }
      } else {
        pwBuf[0] = '\0'; pwLen = 0; shifted = false;
        showKeyboard();
      }
      return;
    }
  }
}

void showKeyboard() {
  wifiState = S_KEYBOARD;
  tft.fillScreen(CLR_BG);
  drawHdr("ENTER PASSWORD", true);

  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextFont(1);
  tft.setTextDatum(ML_DATUM);
  String nl = nets[selNet].ssid;
  if (nl.length() > 50) nl = nl.substring(0, 49) + "~";
  tft.drawString(nl, PW_BOX_X, PW_BOX_Y + 2);

  int pwY = PW_BOX_Y + 12;
  tft.fillRect(PW_BOX_X, pwY, PW_BOX_W, PW_BOX_H, CLR_INPUT);
  tft.drawRect(PW_BOX_X, pwY, PW_BOX_W, PW_BOX_H, CLR_TEXT);
  tft.drawRect(PW_BOX_X + 1, pwY + 1, PW_BOX_W - 2, PW_BOX_H - 2, CLR_FRAME);

  redrawPwBox();

  
  const char (*layout)[KBD_COLS] = shifted ? KU : KL;
  for (int r = 0; r < KBD_ROWS; r++) {
    for (int c = 0; c < KBD_COLS; c++) {
      int kx = KBD_X + c * (KBD_KW + KBD_GAP) + KBD_KW / 2;
      int ky = KBD_Y + r * (KBD_KH + KBD_GAP) + KBD_KH / 2;
      char lbl[2] = { layout[r][c], 0 };
      kbBtn[r][c].initButton(&tft, kx, ky, KBD_KW, KBD_KH,
        CLR_TEXT, CLR_PANEL, CLR_TEXT, lbl, 1);
      tft.setTextFont(2);
      kbBtn[r][c].drawButton();
    }
  }

  
  
  
  
  
  
  

  tft.setTextFont(1);
  int sy   = SPL_Y + 15;   
  int curX = KBD_X;        

  
  int shiftW = 80;
  btnShift.initButton(&tft,
    curX + shiftW / 2, sy,          
    shiftW, 30,
    CLR_TEXT,
    shifted ? CLR_HDR   : CLR_PANEL,
    shifted ? CLR_INVTEXT : CLR_TEXT,
    (char*)"SHIFT", 1);
  btnShift.drawButton();
  curX += shiftW + 4;

  
  int spaceW = 160;
  btnSpace.initButton(&tft,
    curX + spaceW / 2, sy,
    spaceW, 30,
    CLR_TEXT, CLR_PANEL, CLR_TEXT,
    (char*)"SPACE", 1);
  btnSpace.drawButton();
  curX += spaceW + 4;

  
  int delW = 80;
  btnDel.initButton(&tft,
    curX + delW / 2, sy,
    delW, 30,
    CLR_TEXT, CLR_PANEL, CLR_RED,
    (char*)"DEL", 1);
  btnDel.drawButton();
  curX += delW + 4;

  
  
  int connectW = (SW - MARGIN) - curX;
  btnOK.initButton(&tft,
    curX + connectW / 2, sy,
    connectW, 30,
    CLR_TEXT, CLR_HDR, CLR_INVTEXT,
    (char*)"CONNECT", 1);
  btnOK.drawButton();
}

void redrawPwBox() {
  int pwY = PW_BOX_Y + 12;
  tft.fillRect(PW_BOX_X + 2, pwY + 2, PW_BOX_W - 4, PW_BOX_H - 4, CLR_INPUT);
  tft.setTextColor(CLR_TEXT, CLR_INPUT);
  tft.setTextDatum(ML_DATUM);
  tft.setTextFont(2);

  String pw = String(pwBuf);
  
  String display = pw + (kbCursorVisible ? "|" : " ");
  while (display.length() > 1 && tft.textWidth(display) > PW_BOX_W - 16)
    display = display.substring(1);
  tft.drawString(display, PW_BOX_X + 6, pwY + PW_BOX_H / 2);

  tft.setTextColor(CLR_SUB, CLR_INPUT);
  tft.setTextDatum(MR_DATUM);
  tft.setTextFont(1);
  tft.drawString(String(pwLen) + "/64", PW_BOX_X + PW_BOX_W - 4, pwY + PW_BOX_H / 2);
}

void handleKbdTouch(uint16_t tx, uint16_t ty) {
  static uint32_t last = 0;
  if (millis() - last < 100) return;
  last = millis();

  if (tx < 60 && ty < HDR_H) {
    pwBuf[0] = '\0'; pwLen = 0; selNet = -1;
    showList(); return;
  }

  
  btnShift.press(btnShift.contains(tx, ty));
  btnSpace.press(btnSpace.contains(tx, ty));
  btnDel.press(btnDel.contains(tx, ty));
  btnOK.press(btnOK.contains(tx, ty));

  if (btnShift.justPressed()) {
    btnShift.press(false);   
    shifted = !shifted; showKeyboard(); return;
  }
  if (btnSpace.justPressed()) {
    btnSpace.press(false);
    if (pwLen < 64) { pwBuf[pwLen++] = ' '; pwBuf[pwLen] = '\0'; redrawPwBox(); }
    return;
  }
  if (btnDel.justPressed()) {
    btnDel.press(false);
    if (pwLen > 0) { pwBuf[--pwLen] = '\0'; redrawPwBox(); }
    return;
  }
  if (btnOK.justPressed()) {
    btnOK.press(false);
    String ssid = nets[selNet].ssid;
    String pass = String(pwBuf);
    showConnecting();
    if (doConnect(ssid, pass)) {
      saveCreds(ssid, pass);
      showDone();
      wifiState = S_DONE;
      pmoMode = true;
      initPMO();
    } else {
      tft.fillScreen(CLR_BG);
      drawHdr("CONNECTION FAILED");
      tft.setTextColor(CLR_RED, CLR_BG);
      tft.setTextDatum(MC_DATUM);
      tft.setTextFont(2);
      tft.drawString("Wrong password or timeout.", SW / 2, SH / 2 - 10);
      tft.setTextColor(CLR_TEXT, CLR_BG);
      tft.drawString("Returning to password entry...", SW / 2, SH / 2 + 18);
      delay(2200);
      showKeyboard();
    }
    return;
  }

  
  const char (*layout)[KBD_COLS] = shifted ? KU : KL;
  for (int r = 0; r < KBD_ROWS; r++) {
    for (int c = 0; c < KBD_COLS; c++) {
      kbBtn[r][c].press(kbBtn[r][c].contains(tx, ty));
      if (kbBtn[r][c].justPressed()) {
        tft.setTextFont(2);
        kbBtn[r][c].drawButton(true);
        if (pwLen < 64) { pwBuf[pwLen++] = layout[r][c]; pwBuf[pwLen] = '\0'; redrawPwBox(); }
        delay(70);
        kbBtn[r][c].drawButton(false);
        kbBtn[r][c].press(false);   
        if (shifted) { shifted = false; showKeyboard(); }
        return;
      }
    }
  }
}

void doScan() {
  wifiState = S_SCAN;
  showBoot("Scanning WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(80);

  int n = WiFi.scanNetworks();
  netCount = 0;
  for (int i = 0; i < n && netCount < 20; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int j = 0; j < netCount; j++) if (nets[j].ssid == s) { dup = true; break; }
    if (!dup) nets[netCount++] = { s, WiFi.RSSI(i),
      WiFi.encryptionType(i) != WIFI_AUTH_OPEN };
  }
  
  for (int i = 0; i < netCount - 1; i++)
    for (int j = i + 1; j < netCount; j++)
      if (nets[j].rssi > nets[i].rssi) std::swap(nets[i], nets[j]);
  WiFi.scanDelete();

  listOffset = 0; selNet = -1;
  showList();
}

bool doConnect(const String &ssid, const String &pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  int dot = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) { WiFi.disconnect(); return false; }
    tft.setTextColor(CLR_ACCENT, CLR_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    String dots = "";
    for (int i = 0; i < (dot % 4); i++) dots += ".";
    tft.drawString(dots + "    ", SW / 2, SH / 2 + 40);
    dot++;
    delay(350);
  }
  return true;
}

bool loadCreds(String &s, String &p) {
  if (!SPIFFS.exists(WIFI_CRED_FILE)) return false;
  File f = SPIFFS.open(WIFI_CRED_FILE, "r");
  if (!f) return false;
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, f)) { f.close(); return false; }
  f.close();
  s = doc["ssid"] | "";
  p = doc["pass"] | "";
  return s.length() > 0;
}

void saveCreds(const String &s, const String &p) {
  File f = SPIFFS.open(WIFI_CRED_FILE, "w");
  if (!f) return;
  StaticJsonDocument<256> doc;
  doc["ssid"] = s; doc["pass"] = p;
  serializeJson(doc, f);
  f.close();
}

void updateOLED() {
  oled.clearDisplay();

  
  oled.fillRect(0, 0, 128, 64, SSD1306_WHITE);

  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo);

  
  
  char timeBuf[10];
  if (hasTime) {
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", 
             hour12, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "--:--:--");
  }

  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(2);
  int timeW = strlen(timeBuf) * 12;   
  oled.setCursor((128 - timeW) / 2, 4);
  oled.print(timeBuf);

  
  if (hasTime) {
    const char* ampm = timeinfo.tm_hour >= 12 ? "PM" : "AM";
    oled.setTextSize(1);
    int tagX = (128 + timeW) / 2 + 2;
    oled.setCursor(tagX, 6);
    oled.print(ampm);
  }

  
  oled.drawLine(14, 24, 114, 24, SSD1306_BLACK);

  
  
  char dateBuf[20];
  if (hasTime) {
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d %Y", &timeinfo);
  } else {
    snprintf(dateBuf, sizeof(dateBuf), "Syncing...");
  }

  oled.setTextSize(1);
  int dateW = strlen(dateBuf) * 6;    
  oled.setCursor((128 - dateW) / 2, 29);
  oled.print(dateBuf);

  
  oled.drawLine(14, 40, 114, 40, SSD1306_BLACK);

  
  oled.setTextSize(1);

  const char* relayLabel;
  if (relayTripped) {
    relayLabel = tripReasonStr(tripReason);   
  } else {
    relayLabel = relayState ? "RELAY  ON" : "RELAY  OFF";
  }

  int relayLabelW = strlen(relayLabel) * 6;
  int relayX      = (128 - relayLabelW) / 2;

  if (relayTripped) {
    
    oled.fillRoundRect(relayX - 6, 46, relayLabelW + 12, 13, 3, SSD1306_BLACK);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(relayX, 49);
    oled.print(relayLabel);
  } else if (relayState) {
    
    oled.drawRoundRect(relayX - 6, 46, relayLabelW + 12, 13, 3, SSD1306_BLACK);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(relayX, 49);
    oled.print(relayLabel);
  } else {
    
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(relayX, 49);
    oled.print(relayLabel);
  }

  oled.display();
}

const char* tripReasonStr(TripReason r) {
  switch (r) {
    case TRIP_UNDERVOLT:   return "UnderVolt";
    case TRIP_OVERVOLT:    return "OverVolt";
    case TRIP_OVERCURRENT: return "OverCurr";
    case TRIP_OVERPOWER:   return "OverPower";
    case TRIP_FREQUENCY:   return "Freq Fault";
    case TRIP_LOWPF:       return "Low PF";
    case TRIP_NO_OCCUPANCY: return "No Occupant";
    default:               return "None";
  }
}

void setRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
}

void tripRelay(TripReason reason) {
  uint32_t now = millis();
  if (now - lastTripMs < RELAY_COOLDOWN) return;
  setRelay(false);
  relayTripped = true;
  tripReason   = reason;
  lastTripMs   = now;

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("!! RELAY TRIPPED");
  lcd.setCursor(0, 1);
  String msg = "Cause: ";
  msg += tripReasonStr(reason);
  msg.remove(16);
  while ((int)msg.length() < 16) msg += ' ';
  lcd.print(msg);

  playAlert(reason);
}

bool canRestoreRelay(const Metrics& m) {
  if (!relayTripped) return false;
  switch (tripReason) {
    case TRIP_UNDERVOLT:    return m.voltage   >= RELAY_UV_RESTORE;
    case TRIP_OVERVOLT:     return m.voltage   <= RELAY_OV_RESTORE;
    case TRIP_OVERCURRENT:  return m.current   <= RELAY_OC_RESTORE;
    case TRIP_OVERPOWER:    return m.power     <= RELAY_OP_RESTORE;
    case TRIP_FREQUENCY:    return m.frequency >= RELAY_UF_RESTORE
                                              && m.frequency <= RELAY_OF_RESTORE;
    case TRIP_LOWPF:        return m.pf >= RELAY_LPF_RESTORE || m.power < 50.0f;
    case TRIP_NO_OCCUPANCY: return false;
    default:                return false;
  }
}

/*
 * evaluateProtection — hysteresis-based relay trip and auto-restore logic.
 *
 * HYSTERESIS DESIGN
 *   Every fault has a separate trip threshold and a restore threshold that is
 *   further inside the safe zone. This prevents relay chatter when a reading
 *   hovers right at the trip boundary: the relay won't re-energise until the
 *   measurement is clearly back in range.
 *   Example (undervoltage): trip at <195 V, restore at ≥205 V.
 *
 * COOLDOWN TIMER
 *   After tripping, the relay remains open for at least RELAY_COOLDOWN (5 s)
 *   even if the fault clears immediately. This protects against inrush currents
 *   from loads reconnecting and gives the circuit time to stabilise.
 *
 * EVALUATION ORDER
 *   Faults are checked in priority order:
 *     1. Undervoltage / overvoltage  — supply-side grid faults
 *     2. Overcurrent / overpower     — load-side overload
 *     3. Frequency deviation         — grid instability
 *     4. Low power factor            — only flagged when P > 100 W to ignore
 *        spurious PF readings at near-zero load (PZEM noise floor artefact)
 *
 * RESTORE PATH
 *   canRestoreRelay() checks the restore threshold for whichever fault caused
 *   the trip. A different fault cannot inadvertently release the relay — the
 *   exact trip cause must resolve first.
 */
void evaluateProtection(const Metrics& m) {
  uint32_t now = millis();

  if (relayTripped && (now - lastTripMs >= RELAY_COOLDOWN)) {
    if (canRestoreRelay(m)) {
      setRelay(true);
      relayTripped = false;
      tripReason   = TRIP_NONE;
      playAudioVariant("relayrestored");
    }
    return;
  }
  if (relayTripped) return;

  if (m.voltage   < RELAY_UV_TRIP)                               { tripRelay(TRIP_UNDERVOLT);   return; }
  if (m.voltage   > RELAY_OV_TRIP)                               { tripRelay(TRIP_OVERVOLT);    return; }
  if (m.current   > RELAY_OC_TRIP)                               { tripRelay(TRIP_OVERCURRENT); return; }
  if (m.power     > RELAY_OP_TRIP)                               { tripRelay(TRIP_OVERPOWER);   return; }
  if (m.frequency > RELAY_OF_TRIP || m.frequency < RELAY_UF_TRIP) { tripRelay(TRIP_FREQUENCY);  return; }
  if (m.pf        < RELAY_LPF_TRIP && m.power > 100.0f)         { tripRelay(TRIP_LOWPF);       return; }
}

static String gatherCookie(const char* url) {
  Serial.println("[COOKIE] Fetching anti-bot cookie...");
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32) AppleWebKit/537.36");
  http.setTimeout(8000);
  int code = http.GET();
  String body = http.getString();
  http.end();

  Serial.println("[COOKIE] GET code: " + String(code));

  auto parseHexParam = [&](const String& varName) -> String {
    String search = varName + "=toNumbers(\"";
    int i = body.indexOf(search);
    if (i == -1) return "";
    int s = i + search.length();
    int e = body.indexOf("\"", s);
    return body.substring(s, e);
  };

  String hexA = parseHexParam("a");
  String hexB = parseHexParam("b");
  String hexC = parseHexParam("c");

  if (hexA.length() != 32 || hexB.length() != 32 || hexC.length() != 32) {
    Serial.println("[COOKIE] Could not parse AES params — no anti-bot page?");
    return "";
  }

  auto hexToBytes = [](const String& hex, uint8_t* out) {
    for (int i = 0; i < 16; i++)
      out[i] = strtol(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  };

  uint8_t key[16], iv[16], cipher[16], plain[16];
  hexToBytes(hexA, key);
  hexToBytes(hexB, iv);
  hexToBytes(hexC, cipher);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key, 128);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, 16, iv, cipher, plain);
  mbedtls_aes_free(&aes);

  String cookieVal = "";
  for (int i = 0; i < 16; i++) {
    if (plain[i] < 0x10) cookieVal += "0";
    cookieVal += String(plain[i], HEX);
  }
  Serial.println("[COOKIE] __test = " + cookieVal);
  return cookieVal;
}

static String getOrFetchCookie(const char* url) {
  uint32_t now = millis();
  if (cachedCookie.length() > 0 && (now - cookieFetchedAt) < COOKIE_TTL)
    return cachedCookie;
  cachedCookie    = gatherCookie(url);
  cookieFetchedAt = now;
  return cachedCookie;
}

static void httpTask(void* pvParameters) {
  Serial.println("[HTTP TASK] Started on core " + String(xPortGetCoreID()));

  for (;;) {

    
    if (httpSendPending && WiFi.status() == WL_CONNECTED) {
      httpSendPending = false;

      
      HttpPayload snap;
      if (xSemaphoreTake(payloadMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snap = httpPayload;
        xSemaphoreGive(payloadMutex);
      } else {
        
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      String cookie = getOrFetchCookie(SERVER_URL);
      if (cookie.length() == 0) {
        Serial.println("[HTTP TASK] No cookie — skipping send");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      StaticJsonDocument<512> doc;
      doc["device_id"]      = DEVICE_ID;
      doc["wifi_ssid"]      = snap.wifiSsid;
      doc["voltage"]        = snap.voltage;
      doc["current_a"]      = snap.current;
      doc["power_w"]        = snap.power;
      doc["energy_kwh"]     = snap.energy;
      doc["frequency_hz"]   = snap.frequency;
      doc["power_factor"]   = snap.pf;
      doc["apparent_power"] = snap.apparentPower;
      doc["reactive_power"] = snap.reactivePower;
      doc["load_type"]      = snap.loadType;
      doc["cost_per_hour"]  = snap.costPerHour;
      doc["cost_per_month"] = snap.costPerMonth;
      doc["wasted_power"]   = snap.wastedPower;
      doc["safety_margin"]  = snap.safetyMargin;
      doc["co2_kg"]         = snap.co2Kg;
      doc["relay_state"]    = snap.relayState;
      doc["trip_reason"]    = snap.tripReason;
      String body;
      serializeJson(doc, body);

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.begin(client, SERVER_URL);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Cookie", "__test=" + cookie);
      http.addHeader("User-Agent", "Mozilla/5.0 (ESP32) AppleWebKit/537.36");
      http.setTimeout(8000);

      int code = http.POST(body);
      String resp = http.getString();
      http.end();

      if (resp.indexOf("slowAES") != -1) {
        Serial.println("[HTTP TASK] Anti-bot page — refreshing cookie");
        cachedCookie = "";
      } else {
        Serial.println("[HTTP TASK] POST " + String(code) + " | " + resp);
      }
    }

    
    if (httpPollPending && WiFi.status() == WL_CONNECTED) {
      httpPollPending = false;

      String cookie = getOrFetchCookie(RELAY_POLL_URL);
      if (cookie.length() == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.begin(client, RELAY_POLL_URL);
      http.addHeader("Cookie", "__test=" + cookie);
      http.addHeader("User-Agent", "Mozilla/5.0 (ESP32) AppleWebKit/537.36");
      http.setTimeout(8000);

      int code = http.GET();
      if (code == 200) {
        String resp = http.getString();
        if (resp.indexOf("slowAES") != -1) {
          cachedCookie = "";
        } else {
          StaticJsonDocument<128> jdoc;
          deserializeJson(jdoc, resp);
          if (jdoc["pending"] == true) {
            bool cmd = jdoc["command"] == 1;
            
            
            setRelay(cmd);
            if (!cmd) { relayTripped = false; tripReason = TRIP_NONE; }
            Serial.println("[HTTP TASK] Relay CMD: " + String(cmd ? "ON" : "OFF"));
          }
        }
      }
      http.end();
    }

    
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool jpegDrawCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bmp);
  return 1;
}

void drawSdJpeg(const char* path) {
  // sdMutex may be null during early boot before initPMO creates it
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    Serial.printf("[FACE] Missing: %s\n", path);
    return;
  }
  size_t sz = f.size();
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) buf = (uint8_t*)malloc(sz);
  if (!buf) { f.close(); if (sdMutex) xSemaphoreGive(sdMutex); return; }
  f.read(buf, sz);
  f.close();
  if (sdMutex) xSemaphoreGive(sdMutex); // release before decode — TJpgDec doesn't touch SD
  TJpgDec.drawJpg(0, 0, buf, sz);
  free(buf);
}

void updateFace(bool occupied) {
  uint32_t now = millis();

  if (audioPlaying) {
    if (currentFace != FACE_TALKING) {
      currentFace = FACE_TALKING;
      talkFrame   = 1;
      lastTalkMs  = now;
      drawSdJpeg("/talking/1.jpg");
    } else if (now - lastTalkMs >= 20) {
      lastTalkMs = now;
      talkFrame  = (talkFrame == 1) ? 2 : 1;
      char p[32]; snprintf(p, sizeof(p), "/talking/%d.jpg", talkFrame);
      drawSdJpeg(p);
    }
    return;
  }

  // Leaving talking state — fall through to idle
  if (currentFace == FACE_TALKING) {
    currentFace = occupied ? FACE_IDLE_HUMAN : FACE_IDLE_NOHUMAN;
    lastBlinkMs = now; isBlink = false;
    drawSdJpeg(occupied ? "/idle_human/1.jpg" : "/idle_nohuman/1.jpg");
    return;
  }

  // Priority 2: Relay tripped by electrical fault — cycle relay_trip/1-12.jpg every 7s
  // TRIP_NO_OCCUPANCY falls through to idle_nohuman instead
  if (relayTripped && tripReason != TRIP_NO_OCCUPANCY) {
    if (currentFace != FACE_RELAY_TRIP) {
      currentFace      = FACE_RELAY_TRIP;
      relayTripFrame   = 1;
      lastRelayTripMs  = now;
      drawSdJpeg("/relay_trip/1.jpg");
    } else if (now - lastRelayTripMs >= 7000) {
      lastRelayTripMs = now;
      relayTripFrame  = relayTripFrame % 12 + 1;
      char p[32]; snprintf(p, sizeof(p), "/relay_trip/%d.jpg", relayTripFrame);
      drawSdJpeg(p);
    }
    return;
  }

  // Leaving electrical relay trip — show random relay_restored frame for 2s
  if (currentFace == FACE_RELAY_TRIP && (!relayTripped || tripReason == TRIP_NO_OCCUPANCY)) {
    currentFace = FACE_RELAY_RESTORED;
    faceTimerMs = now;
    char p[32]; snprintf(p, sizeof(p), "/relay_restored/%d.jpg", (esp_random() % 4) + 1);
    drawSdJpeg(p);
    return;
  }

  // Priority 3: Relay restored — hold 2s, then idle
  if (currentFace == FACE_RELAY_RESTORED) {
    if (now - faceTimerMs >= 2000) {
      currentFace = occupied ? FACE_IDLE_HUMAN : FACE_IDLE_NOHUMAN;
      lastBlinkMs = now; isBlink = false;
      drawSdJpeg(occupied ? "/idle_human/1.jpg" : "/idle_nohuman/1.jpg");
    }
    return;
  }

  // Priority 4: Touch — hold 2s, then idle
  if (currentFace == FACE_TOUCH) {
    if (now - faceTimerMs >= 2000) {
      currentFace = occupied ? FACE_IDLE_HUMAN : FACE_IDLE_NOHUMAN;
      lastBlinkMs = now; isBlink = false;
      drawSdJpeg(occupied ? "/idle_human/1.jpg" : "/idle_nohuman/1.jpg");
    }
    return;
  }

  // Priority 5: Idle with periodic blink
  FaceState targetIdle = occupied ? FACE_IDLE_HUMAN : FACE_IDLE_NOHUMAN;

  // Enter idle or switch occupancy mode
  if (currentFace != targetIdle) {
    currentFace = targetIdle;
    lastBlinkMs = now; isBlink = false;
    drawSdJpeg(occupied ? "/idle_human/1.jpg" : "/idle_nohuman/1.jpg");
    return;
  }

  // Trigger blink every 7s — pick one random frame from the blink set
  if (!isBlink && now - lastBlinkMs >= 7000) {
    isBlink      = true;
    blinkStartMs = now;
    int fr = occupied ? (int)(esp_random() % 4) + 2   // idle_human: 2-5
                      : (int)(esp_random() % 2) + 2;  // idle_nohuman: 2-3
    char p[40];
    snprintf(p, sizeof(p), occupied ? "/idle_human/%d.jpg" : "/idle_nohuman/%d.jpg", fr);
    drawSdJpeg(p);
    return;
  }

  // Return to base frame after 2s
  if (isBlink && now - blinkStartMs >= 2000) {
    isBlink     = false;
    lastBlinkMs = now;
    drawSdJpeg(occupied ? "/idle_human/1.jpg" : "/idle_nohuman/1.jpg");
  }
}

String fmt(float v, int dec) {
  if (isnan(v)) return "---";
  return String(v, dec);
}

/*
 * calculateMetrics — derive all 13 monitored quantities from the 6 raw PZEM readings.
 *
 * POWER TRIANGLE (IEEE / IEC AC circuit theory)
 *   Apparent power S = V × I  (VA)
 *   Active power   P = from PZEM sensor  (W)
 *   Reactive power Q = √(S² − P²)  (VAR)
 *   Power factor   PF = P / S  (dimensionless, 0–1)
 *
 * s2 is clamped to ≥ 0 before the sqrt to guard against floating-point
 * rounding that can produce tiny negative values when P ≈ S (near-unity PF).
 *
 * LOAD TYPE CLASSIFICATION
 *   Classified by PF thresholds that reflect real appliance categories:
 *     PF > 0.95 → Resistive  (heaters, incandescent bulbs — P ≈ S)
 *     PF > 0.75 → Mixed      (computers, TVs — moderate reactive component)
 *     PF > 0.50 → Inductive  (motors, fans, air-conditioners — large lag current)
 *     PF ≤ 0.50 → Reactive   (heavily capacitive or distorted loads)
 *
 * COST & ENVIRONMENTAL PROJECTIONS
 *   costPerHour  = (P_watts / 1000) × ₱13.8161/kWh
 *   costPerMonth = costPerHour × 24 h × 30 days  (assumes constant load)
 *   wastedPower  = S − P  (VA not converted to useful work)
 *   safetyMargin = (rated_circuit_A − I) / rated_circuit_A × 100  (% headroom)
 *   co2Kg        = cumulative_energy_kWh × 0.6032 kg/kWh  (PH grid emission factor)
 */
Metrics calculateMetrics(float v, float i, float p,
                          float e, float f, float pf) {
  Metrics m;
  m.voltage   = v; m.current  = i; m.power    = p;
  m.energy    = e; m.frequency= f; m.pf       = pf;

  m.apparentPower = v * i;
  float s2 = m.apparentPower * m.apparentPower - p * p;
  m.reactivePower = sqrt(s2 > 0 ? s2 : 0);

  if      (pf > 0.95f) m.loadType = "Resistive";
  else if (pf > 0.75f) m.loadType = "Mixed";
  else if (pf > 0.50f) m.loadType = "Inductive";
  else                 m.loadType = "Reactive";

  m.costPerHour  = (p / 1000.0f) * COST_PER_KWH;
  m.costPerDay   = m.costPerHour * 24.0f;
  m.costPerMonth = m.costPerDay  * 30.0f;

  m.wastedPower        = m.apparentPower - p;
  m.safetyMargin       = ((MAX_CIRCUIT_A - i) / MAX_CIRCUIT_A) * 100.0f;
  m.voltageVariability = 0;
  m.frequencyDeviation = fabsf(f - NOMINAL_HZ);
  m.co2Kg              = e * CO2_PER_KWH;

  return m;
}

void lcdPrint(int row, const char* label, const String& value) {
  String line = String(label) + value;
  line.remove(16);
  while ((int)line.length() < 16) line += ' ';
  lcd.setCursor(0, row);
  lcd.print(line);
}

void updateLCD(const Metrics& m) {
  if (relayTripped) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("!! RELAY TRIPPED");
    String reason = "  " + String(tripReasonStr(tripReason));
    while ((int)reason.length() < 16) reason += ' ';
    reason.remove(16);
    lcd.setCursor(0, 1); lcd.print(reason);
    return;
  }

  lcd.clear();
  switch (lcdScreen) {
    case 0:
      lcdPrint(0, "V: ", fmt(m.voltage, 1) + " V");
      lcdPrint(1, "I: ", fmt(m.current, 3) + " A");
      break;
    case 1:
      lcdPrint(0, "P: ", fmt(m.power,     1) + " W");
      lcdPrint(1, "f: ", fmt(m.frequency, 2) + " Hz");
      break;
    case 2:
      lcdPrint(0, "E: ", fmt(m.energy, 3) + " kWh");
      lcdPrint(1, "PF:", fmt(m.pf,     3));
      break;
    case 3:
      lcdPrint(0, "Load Type:      ", "");
      lcdPrint(1, "  ", m.loadType);
      break;
    case 4:
      lcdPrint(0, "S: ", fmt(m.apparentPower,  1) + " VA");
      lcdPrint(1, "Q: ", fmt(m.reactivePower,  1) + " VAR");
      break;
    case 5:
      lcdPrint(0, "P/hr: \x01",  fmt(m.costPerHour,  2));
      lcdPrint(1, "P/mo: \x01",  fmt(m.costPerMonth, 2));
      break;
    case 6:
      lcdPrint(0, "Waste: ", fmt(m.wastedPower,  1) + " W");
      lcdPrint(1, "Margin:", fmt(m.safetyMargin, 1) + "%");
      break;
    case 7:
      lcdPrint(0, "CO2:  ", fmt(m.co2Kg,              3) + " kg");
      lcdPrint(1, "Fdev: ", fmt(m.frequencyDeviation,  2) + " Hz");
      break;
  }

  if (!lcdPaused) lcdScreen = (lcdScreen + 1) % LCD_SCREENS;  // ← only advance when not paused
}

void serialDump(const Metrics& m) {
  Serial.printf("[RAW] v=%.2f i=%.3f p=%.2f e=%.4f f=%.1f pf=%.3f\n",
                m.voltage, m.current, m.power, m.energy, m.frequency, m.pf);
}

void readCamSerial() {
  while (CAM_SERIAL.available()) {
    String msg = CAM_SERIAL.readStringUntil('\n');
    msg.trim();
    Serial.println("[CAM RX] " + msg);

    if (msg == "PERSON_DETECTED") {
      lastPersonSeen  = millis();
      occupancyActive = true;
      personPresent   = true;
      if (occupancyTripped) {
        occupancyTripped = false;
        relayTripped     = false;
        tripReason       = TRIP_NONE;
        setRelay(true);
        playAudioVariant("occupancyrestored");
      }
    } else if (msg == "NO_PERSON") {
      occupancyActive = true;
      personPresent   = false;
    }
  }

  if (occupancyActive && !occupancyTripped) {
    if (millis() - lastPersonSeen >= OCCUPANCY_TIMEOUT) {
      occupancyTripped = true;
      tripRelay(TRIP_NO_OCCUPANCY);
    }
  }
}

void showAIRecommendations() {
  // Keep the face on screen — just play the audio. The talking face animation
  // will kick in automatically via updateFace() once audioPlaying becomes true.
  playRecommendation();
}

void handleOccupancyPresence() {
  lastPersonSeen  = millis();
  occupancyActive = true;
  personPresent   = true;
  if (occupancyTripped) {
    occupancyTripped = false;
    relayTripped     = false;
    tripReason       = TRIP_NONE;
    setRelay(true);
    playAudioVariant("occupancyrestored");
  }
}

void handleButtons() {
  uint32_t now = millis();

  if (digitalRead(BTN_WIFI) == LOW && now - lastBtnWifi > BTN_DEBOUNCE) {
    lastBtnWifi = now;
    pmoMode     = false;
    wifiState   = S_BOOT;
    SPIFFS.remove(WIFI_CRED_FILE);
    WiFi.disconnect();
    doScan();
  }

  if (digitalRead(BTN_AI) == LOW && now - lastBtnAI > BTN_DEBOUNCE) {
    lastBtnAI = now;
    handleOccupancyPresence();
    showAIRecommendations();
  }

  if (digitalRead(BTN_LCD) == LOW && now - lastBtnLcd > BTN_DEBOUNCE) {
    lastBtnLcd = now;
    handleOccupancyPresence();
    lcdPaused  = !lcdPaused;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(lcdPaused ? "** MONITORING **" : "  LCD CYCLING   ");
    lcd.setCursor(0, 1);
    lcd.print(lcdPaused ? "Screen locked   " : "Screen resumed  ");
    delay(800);
    lcd.clear();
  }
}

void stopAudio() {
  if (audioTaskHandle) {
    vTaskDelete(audioTaskHandle);
    audioTaskHandle = nullptr;
  }
  if (wavGen) {
    if (wavGen->isRunning()) wavGen->stop();
    delete wavGen; wavGen = nullptr;
  }
  if (audioSrc) {
    audioSrc->close();
    delete audioSrc; audioSrc = nullptr;
  }
  audioPlaying = false;
}

void loopAudio() {
  // no-op — audio runs in its own FreeRTOS task now
}

static void audioTask(void* pvParameters) {
  while (audioPlaying && wavGen && wavGen->isRunning()) {
    if (!wavGen->loop()) break;
    vTaskDelay(1);
  }
  if (wavGen)   { wavGen->stop();   delete wavGen;   wavGen   = nullptr; }
  if (audioSrc) { audioSrc->close(); delete audioSrc; audioSrc = nullptr; }
  audioPlaying    = false;
  audioTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void testI2SAudio() {
  Serial.println("[AUDIO] I2S test — playing tone sequence");
  i2sOut->begin();

  const int sampleRate = 24000;
  struct { int hz; int ms; } tones[] = {
    { 440, 250 },
    { 660, 250 },
    { 880, 350 },
  };

  for (auto& t : tones) {
    int n = sampleRate * t.ms / 1000;
    for (int i = 0; i < n; i++) {
      int16_t s     = (int16_t)(sinf(2.0f * PI * t.hz * i / sampleRate) * 16000);
      int16_t lr[2] = { s, s };
      while (!i2sOut->ConsumeSample(lr)) yield();
    }
    delay(60);
  }

  i2sOut->stop();
  Serial.println("[AUDIO] I2S test done");
}

void playAudioVariant(const char* base) {
  stopAudio();
  int variant = (esp_random() % 3) + 1;
  String filename = String(ALERT_FOLDER) + "/" + base + "_" + variant + ".wav";

  audioSrc = new AudioFileSourceSD(filename.c_str());
  if (!audioSrc->isOpen()) {
    Serial.println("[AUDIO] File not found: " + filename);
    delete audioSrc; audioSrc = nullptr;
    return;
  }

  wavGen = new AudioGeneratorWAV();
  if (!wavGen->begin(audioSrc, i2sOut)) {
    Serial.println("[AUDIO] WAV begin failed");
    delete wavGen; wavGen = nullptr;
    delete audioSrc; audioSrc = nullptr;
    return;
  }
  audioPlaying = true;
  Serial.println("[AUDIO] Playing: " + filename);
  xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, nullptr, 2, &audioTaskHandle, 0);
}

void playAlert(TripReason reason) {
  const char* base;
  switch (reason) {
    case TRIP_UNDERVOLT:    base = "undervolt";   break;
    case TRIP_OVERVOLT:     base = "overvolt";    break;
    case TRIP_OVERCURRENT:  base = "overcurrent"; break;
    case TRIP_OVERPOWER:    base = "overpower";   break;
    case TRIP_FREQUENCY:    base = "frequency";   break;
    case TRIP_LOWPF:        base = "lowpf";       break;
    case TRIP_NO_OCCUPANCY: base = "nooccupancy"; break;
    default: return;
  }
  playAudioVariant(base);
}

// Returns the server-side TTS version string ("" on failure / anti-bot).
static String fetchTTSVersion() {
  String cookie = getOrFetchCookie(TTS_INFO_URL);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, TTS_INFO_URL);
  http.addHeader("Cookie", "__test=" + cookie);
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32) AppleWebKit/537.36");
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { http.end(); return ""; }
  String body = http.getString();
  http.end();
  if (body.indexOf("slowAES") != -1) { cachedCookie = ""; return ""; }
  DynamicJsonDocument doc(512);
  deserializeJson(doc, body);
  return doc["version"] | "";
}

// Streams the server-generated WAV directly to SD. Returns true on success.
static bool downloadTTSFromServer() {
  String cookie = getOrFetchCookie(TTS_DL_URL);
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);
  HTTPClient http;
  http.begin(client, TTS_DL_URL);
  http.addHeader("Cookie", "__test=" + cookie);
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32) AppleWebKit/537.36");
  http.setTimeout(30000);
  int code = http.GET();
  if (code != 200) {
    Serial.println("[DL] WAV download failed: " + String(code));
    http.end();
    return false;
  }
  WiFiClient* stream    = http.getStreamPtr();
  int         totalSize = http.getSize();

  // PSRAM path: stream to RAM (no sdMutex held), then write to SD in one quick burst
  const size_t kMaxBuf = 2 * 1024 * 1024;
  uint8_t* dlBuf = (uint8_t*)ps_malloc(kMaxBuf);
  if (dlBuf) {
    uint32_t written  = 0;
    uint32_t deadline = millis() + 30000;
    while (millis() < deadline && written < kMaxBuf) {
      int avail = stream->available();
      if (avail > 0) {
        int n = stream->read(dlBuf + written, min((int)(kMaxBuf - written), avail));
        if (n > 0) written += n;
      } else if (!http.connected()) {
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      if (totalSize > 0 && (int)written >= totalSize) break;
    }
    http.end();

    bool ok = false;
    if (written > 44 && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      File out = SD.open("/tts_recommend.wav", FILE_WRITE);
      if (out) { out.write(dlBuf, written); out.close(); ok = true; }
      xSemaphoreGive(sdMutex);
      Serial.printf("[DL] WAV: %u bytes saved to SD (PSRAM path)\n", written);
    }
    free(dlBuf);
    return ok;
  }

  // Fallback (no PSRAM): release sdMutex between chunk writes
  File out;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    out = SD.open("/tts_recommend.wav", FILE_WRITE);
    xSemaphoreGive(sdMutex);
  }
  if (!out) { http.end(); return false; }

  uint8_t  buf[512];
  uint32_t written  = 0;
  uint32_t deadline = millis() + 30000;
  while (millis() < deadline) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->read(buf, min((int)sizeof(buf), avail));
      if (n > 0) {
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          out.write(buf, n);
          xSemaphoreGive(sdMutex);
        }
        written += n;
      }
    } else if (!http.connected()) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (totalSize > 0 && (int)written >= totalSize) break;
  }
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    out.close();
    xSemaphoreGive(sdMutex);
  }
  http.end();
  bool ok = written > 44;
  Serial.printf("[DL] WAV: %u bytes saved to SD (fallback path)\n", written);
  return ok;
}

void playRecommendation() {
  if (WiFi.status() != WL_CONNECTED) return;
  stopAudio();

  // Wait for any background prefetch already in progress
  {
    uint32_t wStart = millis();
    while (prefetchRunning && millis() - wStart < 90000) vTaskDelay(pdMS_TO_TICKS(200));
  }

  // Always check for a newer recommendation — fast if server version matches SD
  prefetchRecommendation();
  {
    uint32_t wStart = millis();
    while (prefetchRunning && millis() - wStart < 90000) vTaskDelay(pdMS_TO_TICKS(200));
  }

  audioSrc = new AudioFileSourceSD("/tts_recommend.wav");
  if (!audioSrc->isOpen()) {
    Serial.println("[TTS] No WAV on SD");
    delete audioSrc; audioSrc = nullptr;
    return;
  }
  wavGen = new AudioGeneratorWAV();
  if (!wavGen->begin(audioSrc, i2sOut)) {
    Serial.println("[TTS] Playback failed");
    delete wavGen; wavGen = nullptr;
    delete audioSrc; audioSrc = nullptr;
    return;
  }
  audioPlaying = true;
  Serial.println("[TTS] Playing BMO recommendation audio");
  xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, nullptr, 2, &audioTaskHandle, 0);
}

// Calls generate_tts.php on the server. Fast when nothing changed (smart cache).
// May block up to 90s if a new recommendation needs audio generation.
static void triggerTTSGeneration() {
  Serial.println("[PREFETCH] Triggering server-side TTS generation...");
  String cookie = getOrFetchCookie(TTS_GEN_URL);
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(90);
  HTTPClient http;
  http.begin(client, TTS_GEN_URL);
  http.addHeader("Cookie", "__test=" + cookie);
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32) AppleWebKit/537.36");
  http.setTimeout(90000);
  int    code = http.GET();
  String resp = http.getString();
  http.end();
  if (resp.indexOf("slowAES") != -1) cachedCookie = "";
  Serial.println("[PREFETCH] generate_tts: " + String(code) + " " + resp.substring(0, 80));
}

static void prefetchTaskFn(void*) {
  Serial.println("[PREFETCH] Background task on core " + String(xPortGetCoreID()));

  // Step 1 — trigger server-side generation (instant if recommendation unchanged)
  triggerTTSGeneration();

  // Step 2 — read server version and SD cache
  String serverVersion = fetchTTSVersion();
  if (serverVersion.length() == 0) {
    Serial.println("[PREFETCH] Version fetch failed — skipping download");
    prefetchRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  String cachedVersion = "";
  bool   hasWav        = false;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    File vf = SD.open("/tts_version.txt", FILE_READ);
    if (vf) { cachedVersion = vf.readString(); vf.close(); cachedVersion.trim(); }
    hasWav = SD.exists("/tts_recommend.wav");
    xSemaphoreGive(sdMutex);
  }

  // Step 3 — download if version differs or no local WAV
  if (cachedVersion == serverVersion && hasWav) {
    Serial.println("[PREFETCH] Cache up to date — nothing to download");
  } else {
    Serial.println("[PREFETCH] Downloading new WAV from server...");
    if (downloadTTSFromServer()) {
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        File vf = SD.open("/tts_version.txt", FILE_WRITE);
        if (vf) { vf.print(serverVersion); vf.close(); }
        xSemaphoreGive(sdMutex);
      }
      Serial.println("[PREFETCH] Done — new BMO audio ready");
    }
  }

  prefetchRunning = false;
  vTaskDelete(nullptr);
}

// Non-blocking — spawns a Core 0 task so the main loop stays responsive.
void prefetchRecommendation() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (prefetchRunning) { Serial.println("[PREFETCH] Already running"); return; }
  prefetchRunning = true;
  Serial.println("[PREFETCH] Spawning background cache task...");
  xTaskCreatePinnedToCore(prefetchTaskFn, "prefetchTask", 8192, nullptr, 1, nullptr, 0);
}

/*
 * initPMO — run once after a successful WiFi connection to bring up all
 * subsystems in dependency order before the main monitoring loop starts.
 */
void initPMO() {
  Serial.println("[PMO] Initialising energy monitor...");
  sdMutex = xSemaphoreCreateMutex(); // must exist before first drawSdJpeg call

  // Relay starts ON (energised); the protection logic opens it on fault.
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BTN_WIFI, INPUT);
  pinMode(BTN_AI,   INPUT);
  pinMode(BTN_LCD,  INPUT);
  setRelay(true);

  // Sync clock to PH time (UTC+8) so analytics scheduling is accurate.
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  PMO Starting  ");
  lcd.setCursor(0, 1); lcd.print("  WiFi: OK      ");
  delay(800);
  lcd.clear();

  pzemSerial.begin(9600);

  // SD card uses the secondary SPI bus (HSPI) to avoid conflicts with the TFT.
  sdSPI.begin(14, 12, 13, SD_CS); // HSPI: SCK=14, MISO=12, MOSI=13
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("[AUDIO] SD init failed");
  } else {
    Serial.println("[AUDIO] SD ready");
  }

  // Configure I2S DAC for mono 24 kHz WAV playback (matches TTS output format).
  i2sOut = new AudioOutputI2S();
  i2sOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  i2sOut->SetGain(1.5);
  i2sOut->SetRate(24000);
  i2sOut->SetChannels(1);
  testI2SAudio();

  // Load the idle face bitmap from SD card to greet the user on startup.
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegDrawCallback);
  drawSdJpeg("/idle_nohuman/1.jpg");
  lastBlinkMs = millis();

  // Open UART1 to receive occupancy strings ("PERSON_DETECTED" / "NO_PERSON")
  // from the ESP32-CAM. Initialise lastPersonSeen so the 60 s timeout starts
  // from boot rather than from the epoch.
  CAM_SERIAL.begin(CAM_BAUD, SERIAL_8N1, CAM_RX_PIN, -1);
  lastPersonSeen = millis();
  Serial.println("[PMO] Camera UART listener ready on GPIO" + String(CAM_RX_PIN));

  payloadMutex = xSemaphoreCreateMutex();
  audioMutex   = xSemaphoreCreateMutex();

  // Pin the HTTP task to Core 0 so all network I/O runs independently of the
  // sensor-reading and display logic on Core 1.
  xTaskCreatePinnedToCore(
    httpTask,    // task function
    "httpTask",  // name (for debugging)
    8192,        // stack size in bytes
    nullptr,     // no parameters
    1,           // priority
    nullptr,     // no handle needed
    0            // Core 0
  );

  Serial.println("[PMO] HTTP task launched on Core 0.");
  prefetchRecommendation(); // Download latest AI TTS audio in background
  Serial.println("[PMO] Ready.");
}

/*
 * setup — hardware initialisation sequence.
 *
 * Execution order matters here:
 *  1. I2C bus must start before OLED and LCD.
 *  2. SPIFFS must mount before touch calibration (reads /TouchCalData2).
 *  3. Touch calibration must complete before any touch input is processed.
 *  4. If saved WiFi credentials exist and the network is in range,
 *     auto-connect and jump straight to PMO mode. Otherwise show the
 *     touchscreen WiFi manager for manual selection.
 */
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== PMO Energy Monitor ===");

  Wire.begin(SDA_PIN, SCL_PIN);

  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.display();

  lcd.init();
  lcd.createChar(1, pesoChar);
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  PMO Starting  ");
  lcd.setCursor(0, 1); lcd.print(" WiFi setup...  ");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  if (!SPIFFS.begin(true)) Serial.println("[WARN] SPIFFS failed");

  touch_calibrate();

  String ss, pp;
  if (loadCreds(ss, pp)) {
    showBoot(("Looking for: " + ss).c_str());
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);
    bool found = false;
    for (int i = 0; i < n; i++) if (WiFi.SSID(i) == ss) { found = true; break; }
    WiFi.scanDelete();

    if (found) {
      nets[0] = { ss, -60, true };
      netCount = 1; selNet = 0;
      showConnecting();
      if (doConnect(ss, pp)) {
        showDone();
        wifiState = S_DONE;
        pmoMode = true;
        initPMO();
        return;   
      }
    }
    
    showBoot("Saved network not found.");
    delay(900);
  }

  doScan();   
}

/*
 * loop — main task running on Core 1.
 *
 * Two operating modes share this function:
 *
 *   WiFi Manager mode (pmoMode == false)
 *     Handles only the touchscreen WiFi setup UI. Returns immediately
 *     after processing each touch so the cursor blink stays responsive.
 *
 *   PMO mode (pmoMode == true)
 *     Runs all monitoring, protection, display, and scheduling logic.
 *     Key intervals:
 *       2 s  — PZEM sensor read + protection evaluation
 *       3 s  — LCD screen rotation
 *       1 s  — OLED refresh
 *       ~4 s — NILM detection (throttled in dashboard.js, not here)
 *     HTTP sync (3 s) and relay polling (3 s) happen on Core 0 in httpTask.
 */
void loop() {

  // ── WiFi Manager mode ─────────────────────────────────────────
  if (!pmoMode) {
    // Blink the password-entry cursor while the keyboard is visible.
    if (wifiState == S_KEYBOARD && millis() - lastCursorBlink >= CURSOR_BLINK_MS) {
      lastCursorBlink = millis();
      kbCursorVisible = !kbCursorVisible;
      redrawPwBox();
    }

    uint16_t tx = 0, ty = 0;
    bool pressed = tft.getTouch(&tx, &ty);
    if (!pressed) return;
    if      (wifiState == S_LIST)     handleListTouch(tx, ty);
    else if (wifiState == S_KEYBOARD) handleKbdTouch(tx, ty);
    delay(20);
    return;
  }

  // ── PMO monitoring mode ───────────────────────────────────────
  uint32_t now = millis();
  static uint32_t lastOled       = 0;
  static bool     prefetchedAt6AM = false;
  static bool     prefetchedAt6PM = false;

  // Poll the touchscreen — a tap switches the TFT to the FACE_TOUCH animation
  // and resets the occupancy presence timer (counts as a person present).
  uint16_t tx, ty;
  bool touched = tft.getTouch(&tx, &ty, 300);

  if (touched && currentFace != FACE_TOUCH) {
    currentFace = FACE_TOUCH;
    faceTimerMs = now;
    handleOccupancyPresence();
    char touchPath[24];
    snprintf(touchPath, sizeof(touchPath), "/touch/%d.jpg", (esp_random() % 5) + 1);
    drawSdJpeg(touchPath);
  }
  updateFace(!(relayTripped && tripReason == TRIP_NO_OCCUPANCY));

  if (now - lastOled >= 1000) {   
    lastOled = now;
    if (pmoMode) updateOLED();
  }

  readCamSerial();
  handleButtons();
  loopAudio();

  // Scheduled AI pre-fetch at 6:00 AM and 6:00 PM
  {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      if (ti.tm_hour == 6 && ti.tm_min == 0 && !prefetchedAt6AM) {
        prefetchedAt6AM = true;
        prefetchedAt6PM = false;
        prefetchRecommendation();
      } else if (ti.tm_hour == 18 && ti.tm_min == 0 && !prefetchedAt6PM) {
        prefetchedAt6PM = true;
        prefetchedAt6AM = false;
        prefetchRecommendation();
      }
      if (ti.tm_hour != 6)  prefetchedAt6AM = false;
      if (ti.tm_hour != 18) prefetchedAt6PM = false;
    }
  }

  // ── PZEM sensor read (every 2 s) ─────────────────────────────
  // isnan(v) check guards against the sensor returning NaN when it is still
  // warming up or when the AC circuit has no load — avoids false relay trips.
  if (now - lastPzem >= PZEM_INTERVAL) {
    lastPzem = now;
    float v  = pzem.voltage();
    float i  = pzem.current();
    float p  = pzem.power();
    float e  = pzem.energy();
    float f  = pzem.frequency();
    float pf = pzem.pf();

    if (!isnan(v)) {
      live = calculateMetrics(v, i, p, e, f, pf);
      evaluateProtection(live);
      serialDump(live);
    }
  }

  // ── Cloud sync (every 3 s) ────────────────────────────────────
  // Copy live metrics into the shared HttpPayload struct under the mutex,
  // then set flags for the Core 0 httpTask to send and poll asynchronously.
  if (now - lastServer >= SERVER_INTERVAL) {
    lastServer = now;
    if (!isnan(live.voltage)) {
      if (xSemaphoreTake(payloadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        httpPayload.voltage       = live.voltage;
        httpPayload.current       = live.current;
        httpPayload.power         = live.power;
        httpPayload.energy        = live.energy;
        httpPayload.frequency     = live.frequency;
        httpPayload.pf            = live.pf;
        httpPayload.apparentPower = live.apparentPower;
        httpPayload.reactivePower = live.reactivePower;
        httpPayload.costPerHour   = live.costPerHour;
        httpPayload.costPerMonth  = live.costPerMonth;
        httpPayload.wastedPower   = live.wastedPower;
        httpPayload.safetyMargin  = live.safetyMargin;
        httpPayload.co2Kg         = live.co2Kg;
        strncpy(httpPayload.loadType,   live.loadType.c_str(),         15);
        strncpy(httpPayload.wifiSsid,   WiFi.SSID().c_str(),           32);
        strncpy(httpPayload.tripReason, tripReasonStr(tripReason),     15);
        httpPayload.relayState = relayState ? 1 : 0;
        xSemaphoreGive(payloadMutex);
      }
      httpSendPending = true;  
    }
    httpPollPending = true;    
  }

  if (now - lastLcd >= LCD_INTERVAL) {
    lastLcd = now;
    updateLCD(live);
  }
}