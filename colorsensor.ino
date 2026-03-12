// =============================================================================
//  colorsensor.ino  —  ESP32-C3 | MFRC522 (SPI) + TCS3472 (I2C) | WiFi UI
//  Libs: MFRC522, Adafruit_TCS34725, ArduinoOTA, ESPmDNS, Preferences
// =============================================================================

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_TCS34725.h>
#include <Secrets.h>   // defines MYSSID, MYPSK
#include "html.h"

// ── Debug toggle (set 0 to disable all serial output) ────────────────────────
#define DEBUG 1
#if DEBUG
  #define DBG(x)   Serial.print(x)
  #define DBGLN(x) Serial.println(x)
#else
  #define DBG(x)
  #define DBGLN(x)
#endif

// ── Identity ──────────────────────────────────────────────────────────────────
#define HOSTNAME "colorsensor"

// ── Timing ────────────────────────────────────────────────────────────────────
#define WIFI_TIMEOUT_MS  12000UL
#define RECONNECT_MS      5000UL

// ── RFID — SPI (custom pins) ──────────────────────────────────────────────────
#define RFID_SS    7
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10
#define RFID_RST   1

// ── TCS3472 — I2C ─────────────────────────────────────────────────────────────
#define COLOR_SDA  4
#define COLOR_SCL  6

// ── Color event detection tuning ─────────────────────────────────────────────
// At 2.4ms + 16x gain: max ~4096 counts. Tune THRESH to ~5-10% of your white baseline.
#define COLOR_DEVIATION_THRESH  30    // raw counts — lower = more sensitive
#define COLOR_SETTLE_MS         80    // ms back at baseline before committing event
#define EMA_ALPHA               0.05f // baseline drift rate

// ── Objects ───────────────────────────────────────────────────────────────────
Preferences       prefs;
WebServer         server(80);
DNSServer         dns;
MFRC522           rfid(RFID_SS, RFID_RST);
// 2.4ms integration + 16x gain: fast sampling with enough sensitivity for typical LEDs
// If still saturating (counts near 4096) drop to TCS34725_GAIN_4X
// If still too low, increase to TCS34725_GAIN_60X
Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_2_4MS, TCS34725_GAIN_16X);

bool     apMode  = false;
bool     tcsOK   = false;
String   lastUID = "None";

// Live sensor values — always updated every poll
uint16_t cR = 0, cG = 0, cB = 0, cC = 0;

// Live raw values (never overwritten by event commit, always current)
uint16_t liveR = 0, liveG = 0, liveB = 0, liveC = 0;

// ── Baseline + event state ────────────────────────────────────────────────────
float    baseR = 512, baseG = 512, baseB = 512;
bool     inColorEvent     = false;
uint32_t eventSettleStart = 0;
uint16_t peakR = 0, peakG = 0, peakB = 0, peakC = 0;
float    peakDeviation    = 0;

// ── Calibration ─────────────────────────────────────────────────────────────────
struct CalData {
  uint16_t dR, dG, dB;
  uint16_t wR, wG, wB;
  bool valid = false;
};
CalData cal;

void loadCal() {
  prefs.begin("cal", true);
  cal.dR    = prefs.getUShort("dR", 0);
  cal.dG    = prefs.getUShort("dG", 0);
  cal.dB    = prefs.getUShort("dB", 0);
  cal.wR    = prefs.getUShort("wR", 4095);
  cal.wG    = prefs.getUShort("wG", 4095);
  cal.wB    = prefs.getUShort("wB", 4095);
  cal.valid = prefs.getBool("calOK", false);
  prefs.end();
}

void saveCal() {
  prefs.begin("cal", false);
  prefs.putUShort("dR", cal.dR); prefs.putUShort("dG", cal.dG); prefs.putUShort("dB", cal.dB);
  prefs.putUShort("wR", cal.wR); prefs.putUShort("wG", cal.wG); prefs.putUShort("wB", cal.wB);
  prefs.putBool("calOK", cal.valid);
  prefs.end();
}

uint8_t calCh(uint16_t raw, uint16_t dark, uint16_t white) {
  if (white <= dark) return 0;
  return (uint8_t)constrain((int32_t)(raw - dark) * 255 / (white - dark), 0, 255);
}

// ── WiFi helpers ──────────────────────────────────────────────────────────────
bool connectWiFi(const String &ssid, const String &psk) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(ssid.c_str(), psk.c_str());
  DBG("Connecting to "); DBGLN(ssid);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) {
    delay(250); DBG('.');
  }
  DBGLN();
  if (WiFi.status() == WL_CONNECTED) { DBG("IP: "); DBGLN(WiFi.localIP()); return true; }
  DBGLN("WiFi failed"); return false;
}

void startCaptivePortal() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(HOSTNAME);
  dns.start(53, "*", WiFi.softAPIP());
  DBG("AP IP: "); DBGLN(WiFi.softAPIP());
}

void startNetServices() {
  MDNS.end();
  MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]()              { DBGLN("OTA start");           });
  ArduinoOTA.onError([](ota_error_t e) { DBG("OTA error "); DBGLN(e); });
  ArduinoOTA.begin();
  DBGLN("mDNS + OTA ready → " HOSTNAME ".local");
}

// ── Web routes ────────────────────────────────────────────────────────────────
void handleRoot() {
  server.send_P(200, "text/html", apMode ? WIFI_HTML : INDEX_HTML);
}

// Committed color — last peak from a color event (shown as swatch)
void handleData() {
  uint8_t R, G, B;
  if (cal.valid) {
    R = calCh(cR, cal.dR, cal.wR);
    G = calCh(cG, cal.dG, cal.wG);
    B = calCh(cB, cal.dB, cal.wB);
  } else {
    uint32_t s = cC ? cC : 1;
    R = (uint8_t)min(255UL, (uint32_t)cR * 255UL / s);
    G = (uint8_t)min(255UL, (uint32_t)cG * 255UL / s);
    B = (uint8_t)min(255UL, (uint32_t)cB * 255UL / s);
  }
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", R, G, B);
  String j = "{\"r\":"  + String(cR) + ",\"g\":"  + String(cG) +
             ",\"b\":"  + String(cB) + ",\"c\":"  + String(cC) +
             ",\"hex\":\"" + hex + "\",\"uid\":\"" + lastUID + "\"" +
             ",\"calOK\":" + (cal.valid ? "true" : "false") +
             ",\"event\":" + (inColorEvent ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

// Live raw values — always current, used by the live readout strip
void handleLiveData() {
  float dev = max(fabsf((float)liveR - baseR),
             max(fabsf((float)liveG - baseG),
                 fabsf((float)liveB - baseB)));
  String j = "{\"lr\":" + String(liveR) +
             ",\"lg\":" + String(liveG) +
             ",\"lb\":" + String(liveB) +
             ",\"lc\":" + String(liveC) +
             ",\"dev\":" + String((int)dev) +
             ",\"bR\":"  + String((int)baseR) +
             ",\"bG\":"  + String((int)baseG) +
             ",\"bB\":"  + String((int)baseB) +
             ",\"event\":" + (inColorEvent ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void handleSetWifi() {
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing ssid"); return; }
  prefs.begin("wifi", false);
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("psk",  server.arg("psk"));
  prefs.end();
  server.send(200, "text/html",
    "<meta http-equiv='refresh' content='3;url=/'>"
    "<p>Credentials saved — rebooting&hellip;</p>");
  delay(1500);
  ESP.restart();
}

void handleCalBlack() {
  cal.dR = liveR; cal.dG = liveG; cal.dB = liveB;
  saveCal();
  DBG("Cal black: R="); DBG(cal.dR); DBG(" G="); DBG(cal.dG); DBG(" B="); DBGLN(cal.dB);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalWhite() {
  cal.wR = liveR; cal.wG = liveG; cal.wB = liveB;
  cal.valid = true;
  saveCal();
  baseR = cal.wR; baseG = cal.wG; baseB = cal.wB;
  cR = liveR; cG = liveG; cB = liveB; cC = liveC;
  DBG("Cal white: R="); DBG(cal.wR); DBG(" G="); DBG(cal.wG); DBG(" B="); DBGLN(cal.wB);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalReset() {
  prefs.begin("cal", false); prefs.clear(); prefs.end();
  cal = {0, 0, 0, 4095, 4095, 4095, false};
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupServer() {
  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/data",       HTTP_GET,  handleData);
  server.on("/livedata",   HTTP_GET,  handleLiveData);
  server.on("/setwifi",    HTTP_POST, handleSetWifi);
  server.on("/cal/black",  HTTP_POST, handleCalBlack);
  server.on("/cal/white",  HTTP_POST, handleCalWhite);
  server.on("/cal/reset",  HTTP_POST, handleCalReset);
  server.onNotFound([]() {
    server.sendHeader("Location",
      String("http://") + (apMode ? WiFi.softAPIP().toString()
                                  : WiFi.localIP().toString()) + "/");
    server.send(302);
  });
  server.begin();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(400);
#endif
  DBGLN("\n== " HOSTNAME " ==");

  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", MYSSID);
  String psk  = prefs.getString("psk",  MYPSK);
  prefs.end();

  if (connectWiFi(ssid, psk))
    startNetServices();
  else
    startCaptivePortal();

  setupServer();

  // SPI → MFRC522
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  #if DEBUG
    DBG("RFID gain register: 0x");
    DBGLN(String(rfid.PCD_ReadRegister(rfid.RFCfgReg), HEX));
  #endif
  DBGLN("RFID ready");

  // I2C → TCS3472
  Wire.begin(COLOR_SDA, COLOR_SCL);
  tcsOK = tcs.begin();
  loadCal();
  if (cal.valid) { baseR = cal.wR; baseG = cal.wG; baseB = cal.wB; }
  DBGLN(tcsOK ? "TCS3472 ready (2.4ms/16x)" : "TCS3472 NOT found — check wiring");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {

  // ── WiFi reconnect ────────────────────────────────────────────────────────────
  static unsigned long lastReconnect = 0;
  if (!apMode && WiFi.status() != WL_CONNECTED &&
      millis() - lastReconnect > RECONNECT_MS) {
    lastReconnect = millis();
    DBGLN("WiFi lost — reconnecting…");
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", MYSSID);
    String psk  = prefs.getString("psk",  MYPSK);
    prefs.end();
    if (connectWiFi(ssid, psk)) startNetServices();
  }

  if (apMode)  dns.processNextRequest();
  else         ArduinoOTA.handle();
  server.handleClient();

  // ── RFID fast scan (WUPA — wakes all tags including halted) ──────────────────
  {
    static bool tagWasPresent = false;
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);
    MFRC522::StatusCode wakeStatus = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
    if (wakeStatus == MFRC522::STATUS_OK) {
      if (rfid.PICC_ReadCardSerial()) {
        String uid = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
          if (i) uid += ':';
          if (rfid.uid.uidByte[i] < 0x10) uid += '0';
          uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase();
        if (uid != lastUID) {
          lastUID = uid;
          DBG("RFID: "); DBGLN(lastUID);
        }
        tagWasPresent = true;
        rfid.PCD_StopCrypto1();
      }
    } else {
      if (tagWasPresent) {
        tagWasPresent = false;
        //DBGLN("RFID: tag left field");
      }
    }
  }

  // ── Color sensor — always update live values, detect deviation events ────────
  {
    static unsigned long lastColorPoll = 0;
    if (tcsOK && millis() - lastColorPoll >= 3) {
      lastColorPoll = millis();
      uint16_t r, g, b, c;
      tcs.getRawData(&r, &g, &b, &c);

      // Always keep live globals current (used by /livedata and cal captures)
      liveR = r; liveG = g; liveB = b; liveC = c;

      // cR/cG/cB/cC reflect the committed (peak event) color for the swatch;
      // seed them on first valid read so the UI isn't stuck at 0 before any event
      if (cC == 0) { cR = r; cG = g; cB = b; cC = c; }

      float dR = fabsf((float)r - baseR);
      float dG = fabsf((float)g - baseG);
      float dB = fabsf((float)b - baseB);
      float maxDev = max(dR, max(dG, dB));

      if (!inColorEvent) {
        if (maxDev > COLOR_DEVIATION_THRESH) {
          inColorEvent     = true;
          peakDeviation    = maxDev;
          peakR = r; peakG = g; peakB = b; peakC = c;
          eventSettleStart = 0;
          DBG("Color event start dev="); DBGLN(maxDev);
        } else {
          baseR += EMA_ALPHA * ((float)r - baseR);
          baseG += EMA_ALPHA * ((float)g - baseG);
          baseB += EMA_ALPHA * ((float)b - baseB);
        }
      } else {
        if (maxDev > peakDeviation) {
          peakDeviation = maxDev;
          peakR = r; peakG = g; peakB = b; peakC = c;
        }
        if (maxDev <= COLOR_DEVIATION_THRESH) {
          if (eventSettleStart == 0) eventSettleStart = millis();
          if (millis() - eventSettleStart >= COLOR_SETTLE_MS) {
            cR = peakR; cG = peakG; cB = peakB; cC = peakC;
            DBG("Color committed R="); DBG(cR); DBG(" G="); DBG(cG); DBG(" B="); DBGLN(cB);
            inColorEvent     = false;
            eventSettleStart = 0;
            peakDeviation    = 0;
            baseR = (float)r; baseG = (float)g; baseB = (float)b;
          }
        } else {
          eventSettleStart = 0;
        }
      }
    }
  }
}
