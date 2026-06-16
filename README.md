# PMO — Power Monitor Operator

> An ESP32-based residential energy monitoring and protection device with a touchscreen WiFi manager, animated face interface, automated relay protection, AI energy analytics, and cloud logging backend.

**Live Demo:** https://pmo.infinityfree.me/

---

## Hardware Used

| Component | Model / Details | Interface |
|---|---|---|
| Microcontroller | ESP32 NodeMCU (240 MHz dual-core) | — |
| Power Sensor | PZEM-004T v3.0 | UART2 |
| Primary Display | ILI9488 480x320 TFT + XPT2046 Touch | SPI (TFT_eSPI `User_Setup`) |
| Secondary Display | SSD1306 128x64 OLED | I2C `0x3C` |
| LCD | 16x2 Character LCD | I2C `0x27` |
| Relay Module | Single-channel, normally-ON | GPIO 32 |
| Audio DAC | I2S DAC module | GPIO 26 (BCLK), 27 (LRC), 5 (DOUT) |
| SD Card | FAT32 MicroSD via SPI | HSPI (CS = GPIO 25) |
| Occupancy Camera | ESP32-CAM + Edge Impulse CNN | UART1 (RX = GPIO 36, 9600 baud) |

### GPIO Pin Map

| Signal | GPIO |
|---|---|
| Relay | 32 |
| I2C SDA | 21 |
| I2C SCL | 22 |
| PZEM RX | 16 |
| PZEM TX | 17 |
| I2S BCLK | 26 |
| I2S LRC | 27 |
| I2S DOUT | 5 |
| SD Card CS | 25 |
| Button - WiFi | 39 |
| Button - AI | 34 |
| Button - LCD | 35 |
| CAM RX | 36 |

---

## Libraries Required for Installation

### Arduino / ESP32 Firmware

Install these libraries via **Arduino IDE > Sketch > Include Library > Manage Libraries**, or through the `.zip` import for GitHub-only releases.

| Library | Version | Purpose |
|---|---|---|
| `TFT_eSPI` | >= 2.5 | ILI9488 TFT display + XPT2046 touch controller |
| `PZEM004Tv30` | >= 3.0 | PZEM-004T power sensor over UART |
| `LiquidCrystal_I2C` | any | 16x2 I2C LCD |
| `Adafruit GFX Library` | >= 1.11 | Graphics primitives for OLED |
| `Adafruit SSD1306` | >= 2.5 | SSD1306 128x64 OLED driver |
| `ArduinoJson` | >= 6.x | JSON serialization for cloud payloads |
| `ESP8266Audio` | >= 1.9 | AudioGeneratorWAV, AudioFileSourceSD, AudioOutputI2S |
| `TJpg_Decoder` | any | JPEG decoding for SD card images |
| `Edge Impulse SDK` | latest | Person detection CNN inference on ESP32-CAM |
| Built-in: `WiFi`, `HTTPClient`, `WiFiClientSecure` | ESP32 core | HTTPS cloud communication |
| Built-in: `SPIFFS` | ESP32 core | Persistent credential and calibration storage |
| Built-in: `mbedtls/aes.h` | ESP32 core | AES-128-CBC decryption for anti-bot cookie |
| Built-in: `Wire`, `SPI`, `time.h` | ESP32 core | I2C / SPI bus, NTP time sync |

> **TFT_eSPI note:** Edit `User_Setup.h` inside the library folder to enable `ILI9488_DRIVER` and configure your SPI pins before compiling.

> **Edge Impulse note:** Train and export a person-detection model from Edge Impulse Studio as an Arduino library, then install it via `.zip`. Flash `sketch_cam/sketch_cam.ino` onto the ESP32-CAM separately.

### Backend (Web server)

| Requirement | Version |
|---|---|
| PHP | 7.2 or higher |
| MySQL | 5.7 or higher |
| phpMyAdmin | any (for database management) |
| MySQLi extension | enabled |
| cURL extension | enabled (for Gemini API calls) |

### Frontend (browser)

No build step required. The frontend uses a CDN-loaded library.

| Library | Version | Source |
|---|---|---|
| Chart.js | 4.4.1 | CDN in `<head>` of each HTML page |

---

## How to Run

### Step 1 — Power On and Connect to WiFi

1. Power on the ESP32. The TFT screen displays **PMO WiFi Setup**.
2. The device scans nearby networks and lists them on the touchscreen. Signal strength is shown as `***` (strong), `**` (medium), or `*` (weak). Networks that require a password are marked with `[P]`.
3. Tap your network name to select it.
4. If the network requires a password, an on-screen keyboard appears. Type the password and tap **CONNECT**.
5. The device attempts to connect. On success it briefly shows **CONNECTED** then transitions to PMO Mode.
6. WiFi credentials are saved automatically. On the next power-on the device will reconnect to the same network without showing the setup screen again.

### Step 2 — Monitor the Device Locally

Once in PMO Mode, all three displays become active:

- **TFT screen** — shows the animated PMO face. Tap the screen at any time for a touch animation.
- **16x2 LCD** — rotates through 8 metric screens every 3 seconds: voltage/current, power/frequency, energy/power factor, load type, apparent/reactive power, cost per hour/month, wasted power/safety margin, and CO2/frequency deviation.
- **OLED** — shows live voltage, current, relay state, and any active trip reason.

The three physical buttons provide additional control:
- **WiFi button (GPIO 39)** — re-opens the WiFi manager to change networks.
- **AI button (GPIO 34)** — plays the latest AI energy recommendation through the speaker.
- **LCD button (GPIO 35)** — pauses or resumes the LCD screen rotation.

### Step 3 — Access the Web Dashboard

Open a browser and go to your deployed web address (e.g. `https://pmo.infinityfree.me/`).

The dashboard updates automatically every second. It shows all 13 metrics, relay status, a 4-chart history view, and the AI energy analytics panel.

- **Relay control** — use the ON/OFF buttons on the dashboard to remotely open or close the relay. Commands are queued in the database and executed within 3 seconds by the device.
- **AI analytics** — the energy rating, predicted monthly bill, and three recommendations are updated automatically at **6:00 AM** and **6:00 PM** (Philippine Time).
- **NILM (appliance detection)** — go to the NILM page, enter a device name, turn the appliance on, and tap **Start Registration**. The system samples for 30 seconds and stores the appliance signature. Active devices are then identified automatically from the live readings.

### Step 4 — Relay Protection (automatic)

The relay operates automatically and requires no user action during normal use. It will trip (cut power) when any of the following conditions are detected:

| Fault | Trip condition | Restore condition |
|---|---|---|
| Undervoltage | < 195 V | >= 205 V |
| Overvoltage | > 250 V | <= 240 V |
| Overcurrent | > 15 A | <= 13.5 A |
| Overpower | > 3200 W | <= 3000 W |
| Under-frequency | < 57 Hz | >= 58.5 Hz |
| Over-frequency | > 63 Hz | <= 61.5 Hz |
| Low power factor | < 0.50 (at load > 100 W) | >= 0.65 or load < 50 W |
| No occupancy | Room empty for 60 s | Person detected by camera |

The LCD and dashboard both display the trip reason. The relay auto-restores 5 seconds after the fault clears. It can also be manually restored from the web dashboard at any time.

---

## Features at a Glance

| Feature | Details |
|---|---|
| Real-time monitoring | 13 electrical metrics at 2-second resolution |
| Automated protection | 7 fault criteria with hysteresis relay trip/restore |
| Remote relay control | Web-queued ON/OFF commands polled every 3 seconds |
| AI analytics | Gemini 1.5 Flash - energy rating, bill prediction, 3 recommendations |
| NILM | Appliance identification via exhaustive subset search on power/current/PF signatures |
| TTS audio | BMO-personality voice recommendations, cached as WAV on SD card |
| Occupancy detection | ESP32-CAM person detection via Edge Impulse CNN |

---

## Project Structure

```
PMO/
├── sketch_pmo/sketch_pmo.ino   # ESP32 firmware (FreeRTOS, PZEM, TFT, relay, audio)
├── sketch_cam/sketch_cam.ino   # ESP32-CAM occupancy detection
├── htdocs/htdocs/              # Web application root (PHP + HTML + JS + CSS)
│   ├── index.html              # Main dashboard
│   ├── monitor.html            # 13-metric live grid
│   ├── history.html            # Charts and data log
│   ├── nilm.html               # Appliance registry and detection
│   ├── js/dashboard.js         # All frontend polling and UI logic
│   ├── css/styles.css          # Design system
│   └── sql/                    # Database schema
└── sd_card/                    # Audio alert files for SD card
```