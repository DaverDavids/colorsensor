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
#define WIFI_TIMEOUT_MS  12000UL   // connect attempt window
#define RECONNECT_MS      5000UL   // retry interval when disconnected
#define COLOR_INTERVAL_MS  100UL   // sensor poll rate

// ── RFID — SPI (custom pins) ──────────────────────────────────────────────────
#define RFID_SS    7    // RC522 "SDA" = SPI CS
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10
#define RFID_RST   1

// ── TCS3472 — I2C ─────────────────────────────────────────────────────────────
#define COLOR_SDA  4
#define COLOR_SCL  6

// ── Objects ───────────────────────────────────────────────────────────────────
Preferences       prefs;
WebServer         server(80);
DNSServer         dns;
MFRC522           rfid(RFID_SS, RFID_RST);
Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);

bool     apMode  = false;
bool     tcsOK   = false;
String   lastUID = "None";
uint16_t cR = 0, cG = 0, cB = 0, cC = 0;

// ── Calibration (2-point: black + white per channel) ─────────────────────────
struct CalData {
  uint16_t dR, dG, dB;          // dark (floor) values
  uint16_t wR, wG, wB;          // white (ceiling) values
  bool valid = false;
};
CalData cal;

void loadCal() {
  prefs.begin("cal", true);
  cal.dR    = prefs.getUShort("dR", 0);
  cal.dG    = prefs.getUShort("dG", 0);
  cal.dB    = prefs.getUShort("dB", 0);
  cal.wR    = prefs.getUShort("wR", 65535);
  cal.wG    = prefs.getUShort("wG", 65535);
  cal.wB    = prefs.getUShort("wB", 65535);
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

// Idempotent — safe to call on reconnect
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

void handleData() {
  uint8_t R, G, B;
  if (cal.valid) {
    R = calCh(cR, cal.dR, cal.wR);
    G = calCh(cG, cal.dG, cal.wG);
    B = calCh(cB, cal.dB, cal.wB);
  } else {
    // Uncalibrated fallback — clear channel normalization
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
             ",\"calOK\":" + (cal.valid ? "true" : "false") + "}";
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
  // Capture with sensor covered or pointing at matte black
  cal.dR = cR; cal.dG = cG; cal.dB = cB;
  saveCal();
  DBGLN("Cal black saved");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalWhite() {
  // Capture pointing at matte white paper under your normal lighting
  cal.wR = cR; cal.wG = cG; cal.wB = cB;
  cal.valid = true;
  saveCal();
  DBGLN("Cal white saved");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalReset() {
  prefs.begin("cal", false); prefs.clear(); prefs.end();
  cal = {0, 0, 0, 65535, 65535, 65535, false};
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupServer() {
  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/data",    HTTP_GET,  handleData);
  server.on("/setwifi", HTTP_POST, handleSetWifi);
  server.on("/cal/black", HTTP_POST, handleCalBlack);
  server.on("/cal/white", HTTP_POST, handleCalWhite);
  server.on("/cal/reset", HTTP_POST, handleCalReset);

  // Captive portal: redirect anything unknown to root
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

  // Load saved credentials; fall back to compiled-in Secrets.h values
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
  DBGLN("RFID ready");

  // I2C → TCS3472
  Wire.begin(COLOR_SDA, COLOR_SCL);
  tcsOK = tcs.begin();
  loadCal();
  DBGLN(tcsOK ? "TCS3472 ready" : "TCS3472 NOT found — check wiring");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {

  // ── WiFi reconnect (station mode only) ──────────────────────────────────────
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

  // ── RFID scan ────────────────────────────────────────────────────────────────
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    lastUID = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (i) lastUID += ':';
      if (rfid.uid.uidByte[i] < 0x10) lastUID += '0';
      lastUID += String(rfid.uid.uidByte[i], HEX);
    }
    lastUID.toUpperCase();
    DBG("RFID: "); DBGLN(lastUID);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // ── Color sensor (throttled) ─────────────────────────────────────────────────
  static unsigned long lastColor = 0;
  if (tcsOK && millis() - lastColor > COLOR_INTERVAL_MS) {
    tcs.getRawData(&cR, &cG, &cB, &cC);
    lastColor = millis();
  }
}
