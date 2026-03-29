/*
 * PMO — Power Monitoring Operator
 * WiFi Manager (boot) → PMO Mode (runtime)
 *
 * Hardware: ESP32 NodeMCU | ILI9488 480x320 TFT (SPI) | XPT2046 touch
 *           I2C 16x2 LCD | PZEM-004T v3.0 | Relay module
 *
 * Workflow:
 *   1. Boot → Touch calibration (SPIFFS-cached)
 *   2. WiFi Manager → connect / auto-connect from saved creds
 *   3. On success → transition to PMO energy monitoring mode
 *
 * PIN MAP (unchanged from PMO original)
 *   RELAY   GPIO 32       SDA  GPIO 21     SCL  GPIO 22
 *   PZEM RX GPIO 16       PZEM TX GPIO 17
 *   TFT/Touch: managed by TFT_eSPI User_Setup
 */

// ════════════════════════════════════════════════════════════════
//  INCLUDES
// ════════════════════════════════════════════════════════════════
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

// PMO face bitmaps (RGB565, stored in PROGMEM)
#include "idle.h"
#include "blink.h"
#include "touch.h"

// ════════════════════════════════════════════════════════════════
//  PIN MAP
// ════════════════════════════════════════════════════════════════
#define RELAY_PIN   32
#define SDA_PIN     21
#define SCL_PIN     22
#define PZEM_RX     16
#define PZEM_TX     17
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

#define SERVER_URL  "https://pmo.infinityfree.me/insert.php"
#define RELAY_POLL_URL "https://pmo.infinityfree.me/relay.php?device_id=PMO-ESP32-001"
#define DEVICE_ID   "PMO-ESP32-001"
constexpr uint32_t SERVER_INTERVAL = 3000; // send every 5 seconds
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

// Cookie cache
static String    cachedCookie    = "";
static uint32_t  cookieFetchedAt = 0;
constexpr uint32_t COOKIE_TTL   = 6UL * 60UL * 60UL * 1000UL;

// ════════════════════════════════════════════════════════════════
//  PMO CONFIGURATION
// ════════════════════════════════════════════════════════════════
constexpr float MAX_CIRCUIT_A    = 16.0f;
constexpr float NOMINAL_V        = 220.0f;
constexpr float NOMINAL_HZ       = 60.0f;
constexpr float COST_PER_KWH     = 13.8161f;
constexpr float CO2_PER_KWH      = 0.6032f;

// Relay trip / restore thresholds
// Voltage Protection (PH mains ≈ 220–230 V)
constexpr float RELAY_UV_TRIP    = 195.0f;   // trip if voltage too low
constexpr float RELAY_OV_TRIP    = 250.0f;   // trip if voltage too high

constexpr float RELAY_UV_RESTORE = 205.0f;   // restore after voltage stabilizes
constexpr float RELAY_OV_RESTORE = 240.0f;

// Current Protection (depends on circuit rating)
constexpr float RELAY_OC_TRIP    = 15.0f;    // overload trip
constexpr float RELAY_OC_RESTORE = 13.5f;

// Power Protection (depends on breaker / outlet capacity)
constexpr float RELAY_OP_TRIP    = 3200.0f;  // ≈220V * 15A
constexpr float RELAY_OP_RESTORE = 3000.0f;

// Frequency Protection (PH grid = 60 Hz)
constexpr float RELAY_OF_TRIP    = 63.0f;    // over-frequency
constexpr float RELAY_UF_TRIP    = 57.0f;    // under-frequency

constexpr float RELAY_OF_RESTORE = 61.5f;
constexpr float RELAY_UF_RESTORE = 58.5f;

// Power Factor Protection
constexpr float RELAY_LPF_TRIP   = 0.50f;    // very poor PF
constexpr float RELAY_LPF_RESTORE= 0.65f;

constexpr uint32_t RELAY_COOLDOWN= 5000;

constexpr uint32_t PZEM_INTERVAL = 2000;
constexpr uint32_t LCD_INTERVAL  = 3000;
constexpr uint32_t TOUCH_DURATION= 2000;

// ════════════════════════════════════════════════════════════════
//  WIFI MANAGER CONFIGURATION
// ════════════════════════════════════════════════════════════════
#define CALIBRATION_FILE  "/TouchCalData2"
#define WIFI_CRED_FILE    "/wifi_creds.json"
#define REPEAT_CAL         false
#define WIFI_TIMEOUT_MS    9000

// ════════════════════════════════════════════════════════════════
//  SCREEN CONSTANTS
// ════════════════════════════════════════════════════════════════
#define SW  480
#define SH  320

// ── BMO Palette ──────────────────────────────────────────────────
#define CLR_BG      0x8DB3   // sage green
#define CLR_PANEL   0x6CF0   // mid-green card fill
#define CLR_HDR     0x3D4B   // dark teal header
#define CLR_TEXT    0x10C2   // near-black
#define CLR_FRAME   0xC6F8   // cream-green outline
#define CLR_SUB     0x5BCC   // muted subtext
#define CLR_ACCENT  0x4C8D   // OK / accent green
#define CLR_YELLOW  0xD544   // warning yellow
#define CLR_RED     0xB904   // error red
#define CLR_INPUT   0xB677   // input box fill
#define CLR_INVTEXT 0xE79C   // near-white on dark

// ── WiFi list layout ─────────────────────────────────────────────
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

// ════════════════════════════════════════════════════════════════
//  GLOBAL MODE FLAG
// ════════════════════════════════════════════════════════════════
//  false = WiFi Manager active
//  true  = PMO energy monitoring active
bool pmoMode = false;

// ── Keyboard cursor ───────────────────────────────────────────────
bool     kbCursorVisible = true;
uint32_t lastCursorBlink = 0;
constexpr uint32_t CURSOR_BLINK_MS = 500;

// ════════════════════════════════════════════════════════════════
//  HARDWARE OBJECTS  (single shared tft instance)
// ════════════════════════════════════════════════════════════════
TFT_eSPI          tft;
LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial    pzemSerial(2);
PZEM004Tv30       pzem(pzemSerial, PZEM_RX, PZEM_TX);

// ════════════════════════════════════════════════════════════════
//  LCD CUSTOM CHARS
// ════════════════════════════════════════════════════════════════
byte pesoChar[8] = {
  B11110, B10001, B11111, B10001,
  B11110, B10000, B10000, B10000
};

// ════════════════════════════════════════════════════════════════
//  ╔══════════════════════════════════════════════════════════╗
//  ║                 WIFI MANAGER STATE                       ║
//  ╚══════════════════════════════════════════════════════════╝
// ════════════════════════════════════════════════════════════════
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

// ════════════════════════════════════════════════════════════════
//  ╔══════════════════════════════════════════════════════════╗
//  ║                    PMO STATE                             ║
//  ╚══════════════════════════════════════════════════════════╝
// ════════════════════════════════════════════════════════════════

// ── Face ─────────────────────────────────────────────────────────
enum FaceState { FACE_IDLE, FACE_TOUCH, FACE_BLINK };
FaceState currentFace = FACE_IDLE;
uint32_t  touchStartMs = 0;
uint32_t  lastBlinkMs  = 0;
bool      isBlinking   = false;

// ── Metrics ───────────────────────────────────────────────────────
struct Metrics {
  float voltage, current, power, energy, frequency, pf;
  float apparentPower, reactivePower;
  float costPerHour, costPerDay, costPerMonth;
  float wastedPower, safetyMargin;
  float voltageVariability, frequencyDeviation;
  float co2Kg;
  String loadType;
};
Metrics live;

// ── Relay protection ──────────────────────────────────────────────
enum TripReason {
  TRIP_NONE = 0, TRIP_UNDERVOLT, TRIP_OVERVOLT,
  TRIP_OVERCURRENT, TRIP_OVERPOWER, TRIP_FREQUENCY, TRIP_LOWPF
};
bool       relayState   = true;
bool       relayTripped = false;
TripReason tripReason   = TRIP_NONE;
uint32_t   lastTripMs   = 0;

// ── LCD rotation ──────────────────────────────────────────────────
int lcdScreen = 0;
constexpr int LCD_SCREENS = 8;

// ── Timers ────────────────────────────────────────────────────────
uint32_t lastPzem = 0;
uint32_t lastLcd  = 0;

// ════════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════

// WiFi manager
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

// PMO
void initPMO();
void drawFace(FaceState face);
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

// ════════════════════════════════════════════════════════════════
//  ╔══════════════════════════════════════════════════════════╗
//  ║               WIFI MANAGER IMPLEMENTATION                ║
//  ╚══════════════════════════════════════════════════════════╝
// ════════════════════════════════════════════════════════════════

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

// showDone is called by WiFi manager then transitions to PMO
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
  delay(1800);  // brief display before PMO takes over the screen
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

  // ── Draw letter/number rows ───────────────────────────────────
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

  // ── Special row — calculate positions properly ────────────────
  //
  // Total usable width = SW - MARGIN*2 = 480 - 16 = 464 px
  // Layout: [SHIFT=80][gap=4][SPACE=160][gap=4][DEL=80][gap=4][CONNECT=116]
  // Total = 80+4+160+4+80+4+116 = 448 ✓ (fits with 8px side margin)
  //
  // Button initButton(x,y,w,h,...) — x/y are CENTER of the button

  tft.setTextFont(1);
  int sy   = SPL_Y + 15;   // vertical center of special row
  int curX = KBD_X;        // left edge cursor

  // SHIFT — width 80
  int shiftW = 80;
  btnShift.initButton(&tft,
    curX + shiftW / 2, sy,          // center x
    shiftW, 30,
    CLR_TEXT,
    shifted ? CLR_HDR   : CLR_PANEL,
    shifted ? CLR_INVTEXT : CLR_TEXT,
    (char*)"SHIFT", 1);
  btnShift.drawButton();
  curX += shiftW + 4;

  // SPACE — width 160
  int spaceW = 160;
  btnSpace.initButton(&tft,
    curX + spaceW / 2, sy,
    spaceW, 30,
    CLR_TEXT, CLR_PANEL, CLR_TEXT,
    (char*)"SPACE", 1);
  btnSpace.drawButton();
  curX += spaceW + 4;

  // DEL — width 80
  int delW = 80;
  btnDel.initButton(&tft,
    curX + delW / 2, sy,
    delW, 30,
    CLR_TEXT, CLR_PANEL, CLR_RED,
    (char*)"DEL", 1);
  btnDel.drawButton();
  curX += delW + 4;

  // CONNECT — fills remaining space to right edge
  // Right edge = SW - MARGIN = 472, so width = 472 - curX
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
  // Append cursor character if visible
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

  // ── Special keys ─────────────────────────────────────────────
  btnShift.press(btnShift.contains(tx, ty));
  btnSpace.press(btnSpace.contains(tx, ty));
  btnDel.press(btnDel.contains(tx, ty));
  btnOK.press(btnOK.contains(tx, ty));

  if (btnShift.justPressed()) {
    btnShift.press(false);   // reset so next tap fires again
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

  // ── Letter / number keys ──────────────────────────────────────
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
        kbBtn[r][c].press(false);   // ← reset: allows same key to fire next time
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
  // Sort by signal strength
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

// ════════════════════════════════════════════════════════════════
//  ╔══════════════════════════════════════════════════════════╗
//  ║                  PMO IMPLEMENTATION                      ║
//  ╚══════════════════════════════════════════════════════════╝
// ════════════════════════════════════════════════════════════════

void updateOLED() {
  oled.clearDisplay();

  // ── Full white background (light mode) ─────────────────────
  oled.fillRect(0, 0, 128, 64, SSD1306_WHITE);

  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo);

  // ── Time (large, centered) ──────────────────────────────────
  // textSize(2) = 12px tall, each char 12px wide
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
  int timeW = strlen(timeBuf) * 12;   // size2 char = 12px wide
  oled.setCursor((128 - timeW) / 2, 4);
  oled.print(timeBuf);

  // ── AM / PM tag (small, right-aligned to time block) ───────
  if (hasTime) {
    const char* ampm = timeinfo.tm_hour >= 12 ? "PM" : "AM";
    oled.setTextSize(1);
    int tagX = (128 + timeW) / 2 + 2;
    oled.setCursor(tagX, 6);
    oled.print(ampm);
  }

  // ── Thin divider ────────────────────────────────────────────
  oled.drawLine(14, 24, 114, 24, SSD1306_BLACK);

  // ── Date (medium, centered) ─────────────────────────────────
  // Format: Mon, Jan 01 2025
  char dateBuf[20];
  if (hasTime) {
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d %Y", &timeinfo);
  } else {
    snprintf(dateBuf, sizeof(dateBuf), "Syncing...");
  }

  oled.setTextSize(1);
  int dateW = strlen(dateBuf) * 6;    // size1 char = 6px wide
  oled.setCursor((128 - dateW) / 2, 29);
  oled.print(dateBuf);

  // ── Thin divider ────────────────────────────────────────────
  oled.drawLine(14, 40, 114, 40, SSD1306_BLACK);

  // ── Relay state (centered, styled) ──────────────────────────
  oled.setTextSize(1);

  const char* relayLabel;
  if (relayTripped) {
    relayLabel = tripReasonStr(tripReason);   // e.g. "OverVolt"
  } else {
    relayLabel = relayState ? "RELAY  ON" : "RELAY  OFF";
  }

  int relayLabelW = strlen(relayLabel) * 6;
  int relayX      = (128 - relayLabelW) / 2;

  if (relayTripped) {
    // Filled warning pill for trip state
    oled.fillRoundRect(relayX - 6, 46, relayLabelW + 12, 13, 3, SSD1306_BLACK);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(relayX, 49);
    oled.print(relayLabel);
  } else if (relayState) {
    // Outline pill for ON
    oled.drawRoundRect(relayX - 6, 46, relayLabelW + 12, 13, 3, SSD1306_BLACK);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(relayX, 49);
    oled.print(relayLabel);
  } else {
    // Dashed / muted feel for OFF — just plain centered text
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
  Serial.println("[RELAY TRIP] Reason: " + String(tripReasonStr(reason)));

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("!! RELAY TRIPPED");
  lcd.setCursor(0, 1);
  String msg = "Cause: ";
  msg += tripReasonStr(reason);
  msg.remove(16);
  while ((int)msg.length() < 16) msg += ' ';
  lcd.print(msg);
}

bool canRestoreRelay(const Metrics& m) {
  if (!relayTripped) return false;
  switch (tripReason) {
    case TRIP_UNDERVOLT:   return m.voltage   >= RELAY_UV_RESTORE;
    case TRIP_OVERVOLT:    return m.voltage   <= RELAY_OV_RESTORE;
    case TRIP_OVERCURRENT: return m.current   <= RELAY_OC_RESTORE;
    case TRIP_OVERPOWER:   return m.power     <= RELAY_OP_RESTORE;
    case TRIP_FREQUENCY:   return m.frequency >= RELAY_UF_RESTORE
                                             && m.frequency <= RELAY_OF_RESTORE;
    case TRIP_LOWPF:       return m.pf >= RELAY_LPF_RESTORE || m.power < 50.0f;
    default:               return false;
  }
}

void evaluateProtection(const Metrics& m) {
  uint32_t now = millis();

  if (relayTripped && (now - lastTripMs >= RELAY_COOLDOWN)) {
    if (canRestoreRelay(m)) {
      setRelay(true);
      relayTripped = false;
      tripReason   = TRIP_NONE;
      Serial.println("[RELAY RESTORE] Conditions back to safe range.");
    }
    return;
  }
  if (relayTripped) return;

  if (m.voltage   < RELAY_UV_TRIP)                          { tripRelay(TRIP_UNDERVOLT);   return; }
  if (m.voltage   > RELAY_OV_TRIP)                          { tripRelay(TRIP_OVERVOLT);    return; }
  if (m.current   > RELAY_OC_TRIP)                          { tripRelay(TRIP_OVERCURRENT); return; }
  if (m.power     > RELAY_OP_TRIP)                          { tripRelay(TRIP_OVERPOWER);   return; }
  if (m.frequency > RELAY_OF_TRIP || m.frequency < RELAY_UF_TRIP) { tripRelay(TRIP_FREQUENCY); return; }
  if (m.pf        < RELAY_LPF_TRIP && m.power > 100.0f)    { tripRelay(TRIP_LOWPF);       return; }
}

// ── AES cookie bypass ─────────────────────────────────────────────
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

// ── HTTP Task (runs on Core 0, main loop runs on Core 1) ──────────
static void httpTask(void* pvParameters) {
  Serial.println("[HTTP TASK] Started on core " + String(xPortGetCoreID()));

  for (;;) {

    // ── Send sensor data ────────────────────────────────────────
    if (httpSendPending && WiFi.status() == WL_CONNECTED) {
      httpSendPending = false;

      // Safely copy payload under mutex
      HttpPayload snap;
      if (xSemaphoreTake(payloadMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snap = httpPayload;
        xSemaphoreGive(payloadMutex);
      } else {
        // Couldn't get mutex — skip this cycle
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

    // ── Poll relay command ──────────────────────────────────────
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
            // setRelay touches hardware — call it from this task safely
            // GPIO writes are atomic on ESP32 so this is fine
            setRelay(cmd);
            if (!cmd) { relayTripped = false; tripReason = TRIP_NONE; }
            Serial.println("[HTTP TASK] Relay CMD: " + String(cmd ? "ON" : "OFF"));
          }
        }
      }
      http.end();
    }

    // Yield to other tasks — don't busy-wait
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void drawFace(FaceState face) {
  const uint16_t* bmp;
  if      (face == FACE_IDLE)  bmp = Rosto_01;
  else if (face == FACE_TOUCH) bmp = touch2;
  else                         bmp = Rosto_04;

  tft.startWrite();
  tft.setAddrWindow(0, 0, 480, 320);
  for (int y = 0; y < 320; y++) {
    for (int x = 0; x < 480; x++) {
      uint16_t px = pgm_read_word(&bmp[y * 480 + x]);
      uint8_t r = ( px >> 11)         << 3;
      uint8_t g = ((px >>  5) & 0x3F) << 2;
      uint8_t b = ( px        & 0x1F) << 3;
      tft.writeColor(tft.color565(r, g, b), 1);
    }
  }
  tft.endWrite();
  currentFace = face;
}

String fmt(float v, int dec) {
  if (isnan(v)) return "---";
  return String(v, dec);
}

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
  // ── Trip banner takes priority ───────────────────────────────
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

    // ════ CORE METRICS ════════════════════════════════════════

    // ── Screen 0: Voltage & Current ─────────────────────────
    case 0:
      lcdPrint(0, "V: ", fmt(m.voltage, 1) + " V");
      lcdPrint(1, "I: ", fmt(m.current, 3) + " A");
      break;

    // ── Screen 1: Power & Frequency ─────────────────────────
    case 1:
      lcdPrint(0, "P: ", fmt(m.power,     1) + " W");
      lcdPrint(1, "f: ", fmt(m.frequency, 2) + " Hz");
      break;

    // ── Screen 2: Energy & Power Factor ─────────────────────
    case 2:
      lcdPrint(0, "E: ", fmt(m.energy, 3) + " kWh");
      lcdPrint(1, "PF:", fmt(m.pf,     3));
      break;

    // ── Screen 3: Load Type ──────────────────────────────────
    case 3:
      lcdPrint(0, "Load Type:      ", "");
      lcdPrint(1, "  ", m.loadType);
      break;

    // ════ DERIVED METRICS ═════════════════════════════════════

    // ── Screen 4: Apparent & Reactive Power ─────────────────
    case 4:
      lcdPrint(0, "S: ", fmt(m.apparentPower,  1) + " VA");
      lcdPrint(1, "Q: ", fmt(m.reactivePower,  1) + " VAR");
      break;

    // ── Screen 5: Cost per Hour & per Month ─────────────────
    case 5:
      lcdPrint(0, "P/hr: \x01",  fmt(m.costPerHour,  2));
      lcdPrint(1, "P/mo: \x01",  fmt(m.costPerMonth, 2));
      break;

    // ── Screen 6: Wasted Power & Safety Margin ──────────────
    case 6:
      lcdPrint(0, "Waste: ", fmt(m.wastedPower,  1) + " W");
      lcdPrint(1, "Margin:", fmt(m.safetyMargin, 1) + "%");
      break;

    // ── Screen 7: CO2 & Frequency Deviation ─────────────────
    case 7:
      lcdPrint(0, "CO2:  ", fmt(m.co2Kg,              3) + " kg");
      lcdPrint(1, "Fdev: ", fmt(m.frequencyDeviation,  2) + " Hz");
      break;
  }

  lcdScreen = (lcdScreen + 1) % LCD_SCREENS;
}

void serialDump(const Metrics& m) {
  Serial.println("\n===== CORE =====");
  Serial.println("WiFi:      " + WiFi.localIP().toString());
  Serial.println("Relay:     " + String(relayState ? "ON" : "OFF"));
  Serial.println("Tripped:   " + String(relayTripped ? tripReasonStr(tripReason) : "No"));
  Serial.println("Voltage:   " + fmt(m.voltage,   2) + " V");
  Serial.println("Current:   " + fmt(m.current,   3) + " A");
  Serial.println("Power:     " + fmt(m.power,     2) + " W");
  Serial.println("Energy:    " + fmt(m.energy,    4) + " kWh");
  Serial.println("Frequency: " + fmt(m.frequency, 1) + " Hz");
  Serial.println("PF:        " + fmt(m.pf,        3));

  Serial.println("\n===== POWER =====");
  Serial.println("Apparent:  " + fmt(m.apparentPower, 2) + " VA");
  Serial.println("Reactive:  " + fmt(m.reactivePower, 2) + " VAR");
  Serial.println("Load Type: " + m.loadType);

  Serial.println("\n===== COST =====");
  Serial.println("P/hr:  P" + fmt(m.costPerHour,  2));
  Serial.println("P/day: P" + fmt(m.costPerDay,   2));
  Serial.println("P/mo:  P" + fmt(m.costPerMonth, 2));

  Serial.println("\n===== EFFICIENCY =====");
  Serial.println("Wasted: " + fmt(m.wastedPower,  2) + " W");
  Serial.println("Margin: " + fmt(m.safetyMargin, 1) + "%");
  if (m.safetyMargin < 20) Serial.println("! Approaching circuit limit!");

  Serial.println("\n===== POWER QUALITY =====");
  Serial.println("F Dev:  " + fmt(m.frequencyDeviation, 2) + " Hz");

  Serial.println("\n===== ENVIRONMENTAL =====");
  Serial.println("CO2: " + fmt(m.co2Kg, 3) + " kg");
  Serial.println("===========================\n");
}

// Called once when WiFi connection succeeds, before entering pmoMode loop
void initPMO() {
  Serial.println("[PMO] Initialising energy monitor...");

  pinMode(RELAY_PIN, OUTPUT);
  setRelay(true);

  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  PMO Starting  ");
  lcd.setCursor(0, 1); lcd.print("  WiFi: OK      ");
  delay(800);
  lcd.clear();

  pzemSerial.begin(9600);
  drawFace(FACE_IDLE);
  lastBlinkMs = millis();

  // ── Create mutex and HTTP background task ──────────────────
  payloadMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(
    httpTask,       // function
    "httpTask",     // name (for debugging)
    8192,           // stack size (bytes) — HTTP needs plenty
    nullptr,        // parameters
    1,              // priority (1 = low, won't compete with loop)
    nullptr,        // task handle (don't need it)
    0               // ← Core 0. Main Arduino loop() runs on Core 1.
  );

  Serial.println("[PMO] HTTP task launched on Core 0.");
  Serial.println("[PMO] Ready.");
}

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== PMO Energy Monitor ===");

  // Shared hardware init
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

  // SPIFFS for touch cal + WiFi creds
  if (!SPIFFS.begin(true)) Serial.println("[WARN] SPIFFS failed");

  // Touch calibration (must run after tft.init)
  touch_calibrate();

  // ── WiFi Manager boot flow ────────────────────────────────────
  // showBoot("Checking saved network...");
  SPIFFS.remove(WIFI_CRED_FILE);

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
        return;   // skip doScan, go straight to loop()
      }
    }
    // Saved network not reachable — fall through to scan
    showBoot("Saved network not found.");
    delay(900);
  }

  doScan();   // shows list, user picks network
}

// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════
void loop() {

  // ── WiFi Manager mode ─────────────────────────────────────────
  if (!pmoMode) {
    // ── Cursor blink ─────────────────────────────────────────────
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

  // ── PMO mode ──────────────────────────────────────────────────
  uint32_t now = millis();
  static uint32_t lastOled = 0;

  // Touch → face reaction
  uint16_t tx, ty;
  bool touched = tft.getTouch(&tx, &ty, 300);

  if (touched && currentFace != FACE_TOUCH) {
    drawFace(FACE_TOUCH);
    touchStartMs = now;
    isBlinking   = false;
  }
  if (currentFace == FACE_TOUCH && (now - touchStartMs >= TOUCH_DURATION)) {
    drawFace(FACE_IDLE);
    lastBlinkMs = now;
  }
  if (currentFace == FACE_IDLE && !isBlinking && (now - lastBlinkMs >= 7000)) {
    drawFace(FACE_BLINK);
    isBlinking  = true;
    lastBlinkMs = now;
  }
  if (currentFace == FACE_BLINK && isBlinking && (now - lastBlinkMs >= 150)) {
    drawFace(FACE_IDLE);
    isBlinking  = false;
    lastBlinkMs = now;
  }

  if (now - lastOled >= 1000) {   // update every second for the clock
    lastOled = now;
    if (pmoMode) updateOLED();
  }

  // PZEM read
  if (now - lastPzem >= PZEM_INTERVAL) {
    lastPzem = now;
    float v  = pzem.voltage();
    float i  = pzem.current();
    float p  = pzem.power();
    float e  = pzem.energy();
    float f  = pzem.frequency();
    float pf = pzem.pf();

    Serial.printf("[RAW] v=%.2f i=%.3f p=%.2f e=%.4f f=%.1f pf=%.3f\n",
                  v, i, p, e, f, pf);

    if (!isnan(v)) {
      live = calculateMetrics(v, i, p, e, f, pf);
      evaluateProtection(live);
      serialDump(live);
    }
  }

  // Server push + relay poll — just set flags, HTTP task does the work
  if (now - lastServer >= SERVER_INTERVAL) {
    lastServer = now;
    if (!isnan(live.voltage)) {
      // Copy live metrics into shared payload under mutex
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
      httpSendPending = true;  // signal the task
    }
    httpPollPending = true;    // always poll for relay commands
  }

  // LCD rotate
  if (now - lastLcd >= LCD_INTERVAL) {
    lastLcd = now;
    updateLCD(live);
  }
}