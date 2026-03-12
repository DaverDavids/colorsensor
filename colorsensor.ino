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
#include <ArduinoJson.h>
#include <Secrets.h>
#include "html.h"

// ── Debug toggle ───────────────────────────────────────────────────────────────────
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

// ── RFID pins ───────────────────────────────────────────────────────────────────
#define RFID_SS    7
#define RFID_SCK   8
#define RFID_MISO  9
#define RFID_MOSI  10
#define RFID_RST   1

// ── TCS3472 pins ────────────────────────────────────────────────────────────────
#define COLOR_SDA  4
#define COLOR_SCL  6

// ── Detection tuning ───────────────────────────────────────────────────────────
// Deviation is measured on normalised 0-255 channels, not raw counts.
// THRESH: minimum normalised deviation to consider a ball present (tune 5-20).
// SETTLE: ms of stable baseline before an event is committed.
#define DETECT_THRESH   10
#define SETTLE_MS       100
#define EMA_ALPHA       0.04f

// ── Training ───────────────────────────────────────────────────────────────────
#define MAX_PROFILES    16
#define NAME_LEN        20

struct ColorProfile {
  char     name[NAME_LEN];
  uint8_t  r, g, b;      // normalised 0-255 training values
  bool     used = false;
};

ColorProfile profiles[MAX_PROFILES];
uint8_t      numProfiles = 0;

void loadProfiles() {
  prefs.begin("profiles", true);
  numProfiles = prefs.getUChar("count", 0);
  for (uint8_t i = 0; i < numProfiles && i < MAX_PROFILES; i++) {
    String key = "p" + String(i);
    prefs.getBytes(key.c_str(), &profiles[i], sizeof(ColorProfile));
    profiles[i].used = true;
  }
  prefs.end();
  DBG("Loaded profiles: "); DBGLN(numProfiles);
}

void saveProfiles() {
  prefs.begin("profiles", false);
  prefs.putUChar("count", numProfiles);
  for (uint8_t i = 0; i < numProfiles; i++) {
    String key = "p" + String(i);
    prefs.putBytes(key.c_str(), &profiles[i], sizeof(ColorProfile));
  }
  prefs.end();
}

// Euclidean distance in normalised RGB space
float profileDist(const ColorProfile &p, uint8_t r, uint8_t g, uint8_t b) {
  float dr = (float)p.r - r;
  float dg = (float)p.g - g;
  float db = (float)p.b - b;
  return sqrtf(dr*dr + dg*dg + db*db);
}

// Returns index of nearest profile, or -1 if no profiles or dist > maxDist
int matchProfile(uint8_t r, uint8_t g, uint8_t b, float maxDist = 60.0f) {
  int   best  = -1;
  float bestD = maxDist;
  for (uint8_t i = 0; i < numProfiles; i++) {
    float d = profileDist(profiles[i], r, g, b);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

// ── Objects ───────────────────────────────────────────────────────────────────
Preferences       prefs;
WebServer         server(80);
DNSServer         dns;
MFRC522           rfid(RFID_SS, RFID_RST);
Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_2_4MS, TCS34725_GAIN_16X);

bool   apMode = false;
bool   tcsOK  = false;
String lastUID = "None";

// Live raw sensor readings (always current)
uint16_t liveR = 0, liveG = 0, liveB = 0, liveC = 0;

// Normalised live values (0-255)
uint8_t  normR = 0, normG = 0, normB = 0;

// Last committed detection result
uint8_t  detR = 128, detG = 128, detB = 128;
String   detName   = "—";
float    detDist   = 0;
bool     detMatch  = false;

// ── EMA baseline (normalised space) ─────────────────────────────────────────────
float baseR = 128, baseG = 128, baseB = 128;

// ── Event state ─────────────────────────────────────────────────────────────────
bool     inEvent       = false;
uint32_t settleStart   = 0;
uint8_t  peakR = 0, peakG = 0, peakB = 0;
float    peakDev       = 0;

// ── White calibration ──────────────────────────────────────────────────────────
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

void updateNorm() {
  if (cal.valid) {
    normR = calCh(liveR, cal.dR, cal.wR);
    normG = calCh(liveG, cal.dG, cal.wG);
    normB = calCh(liveB, cal.dB, cal.wB);
  } else {
    uint32_t s = liveC ? liveC : 1;
    normR = (uint8_t)min(255UL, (uint32_t)liveR * 255UL / s);
    normG = (uint8_t)min(255UL, (uint32_t)liveG * 255UL / s);
    normB = (uint8_t)min(255UL, (uint32_t)liveB * 255UL / s);
  }
}

// ── WiFi ───────────────────────────────────────────────────────────────────────
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

// Last committed detection result
void handleData() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", detR, detG, detB);
  String j = "{\"r\":" + String(detR) + ",\"g\":" + String(detG) + ",\"b\":" + String(detB) +
             ",\"hex\":\"" + hex + "\"" +
             ",\"name\":\"" + detName + "\"" +
             ",\"dist\":" + String(detDist, 1) +
             ",\"match\":" + (detMatch ? "true" : "false") +
             ",\"uid\":\"" + lastUID + "\"" +
             ",\"calOK\":" + (cal.valid ? "true" : "false") +
             ",\"event\":" + (inEvent ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

// Live normalised + raw values
void handleLiveData() {
  float dR = fabsf((float)normR - baseR);
  float dG = fabsf((float)normG - baseG);
  float dB = fabsf((float)normB - baseB);
  float dev = max(dR, max(dG, dB));
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", normR, normG, normB);
  String j = "{\"lr\":" + String(liveR) + ",\"lg\":" + String(liveG) +
             ",\"lb\":" + String(liveB) + ",\"lc\":" + String(liveC) +
             ",\"nr\":" + String(normR) + ",\"ng\":" + String(normG) +
             ",\"nb\":" + String(normB) +
             ",\"hex\":\"" + hex + "\"" +
             ",\"dev\":" + String((int)dev) +
             ",\"bR\":" + String((int)baseR) +
             ",\"bG\":" + String((int)baseG) +
             ",\"bB\":" + String((int)baseB) +
             ",\"event\":" + (inEvent ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

// List all training profiles as JSON array
void handleProfilesList() {
  String j = "[";
  for (uint8_t i = 0; i < numProfiles; i++) {
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", profiles[i].r, profiles[i].g, profiles[i].b);
    if (i) j += ",";
    j += "{\"i\":" + String(i) +
         ",\"name\":\"" + String(profiles[i].name) + "\"" +
         ",\"r\":" + String(profiles[i].r) +
         ",\"g\":" + String(profiles[i].g) +
         ",\"b\":" + String(profiles[i].b) +
         ",\"hex\":\"" + hex + "\"}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

// Save current live normalised reading as a new named profile
void handleProfileTrain() {
  if (!server.hasArg("name") || server.arg("name").length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Missing name\"}");
    return;
  }
  if (numProfiles >= MAX_PROFILES) {
    server.send(400, "application/json", "{\"error\":\"Max profiles reached\"}");
    return;
  }
  String name = server.arg("name");
  name.trim();
  ColorProfile &p = profiles[numProfiles];
  strncpy(p.name, name.c_str(), NAME_LEN - 1);
  p.name[NAME_LEN - 1] = 0;
  p.r = normR; p.g = normG; p.b = normB;
  p.used = true;
  numProfiles++;
  saveProfiles();
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", p.r, p.g, p.b);
  DBG("Trained "); DBG(p.name); DBG(" R="); DBG(p.r); DBG(" G="); DBG(p.g); DBG(" B="); DBGLN(p.b);
  String j = "{\"ok\":true,\"name\":\"" + String(p.name) + "\",\"hex\":\"" + hex + "\"}";
  server.send(200, "application/json", j);
}

// Delete a profile by index
void handleProfileDelete() {
  if (!server.hasArg("i")) { server.send(400, "application/json", "{\"error\":\"Missing i\"}"); return; }
  uint8_t idx = server.arg("i").toInt();
  if (idx >= numProfiles) { server.send(400, "application/json", "{\"error\":\"Bad index\"}"); return; }
  // Shift remaining profiles down
  for (uint8_t i = idx; i < numProfiles - 1; i++) profiles[i] = profiles[i + 1];
  numProfiles--;
  saveProfiles();
  server.send(200, "application/json", "{\"ok\":true}");
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
  DBG("Cal black R="); DBG(cal.dR); DBG(" G="); DBG(cal.dG); DBG(" B="); DBGLN(cal.dB);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalWhite() {
  cal.wR = liveR; cal.wG = liveG; cal.wB = liveB;
  cal.valid = true;
  saveCal();
  updateNorm();
  baseR = normR; baseG = normG; baseB = normB;
  DBG("Cal white R="); DBG(cal.wR); DBG(" G="); DBG(cal.wG); DBG(" B="); DBGLN(cal.wB);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalReset() {
  prefs.begin("cal", false); prefs.clear(); prefs.end();
  cal = {0, 0, 0, 4095, 4095, 4095, false};
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupServer() {
  server.on("/",               HTTP_GET,  handleRoot);
  server.on("/data",           HTTP_GET,  handleData);
  server.on("/livedata",       HTTP_GET,  handleLiveData);
  server.on("/profiles",       HTTP_GET,  handleProfilesList);
  server.on("/profiles/train", HTTP_POST, handleProfileTrain);
  server.on("/profiles/delete",HTTP_POST, handleProfileDelete);
  server.on("/setwifi",        HTTP_POST, handleSetWifi);
  server.on("/cal/black",      HTTP_POST, handleCalBlack);
  server.on("/cal/white",      HTTP_POST, handleCalWhite);
  server.on("/cal/reset",      HTTP_POST, handleCalReset);
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

  if (connectWiFi(ssid, psk)) startNetServices();
  else                         startCaptivePortal();

  setupServer();

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  #if DEBUG
    DBG("RFID gain: 0x");
    DBGLN(String(rfid.PCD_ReadRegister(rfid.RFCfgReg), HEX));
  #endif
  DBGLN("RFID ready");

  Wire.begin(COLOR_SDA, COLOR_SCL);
  tcsOK = tcs.begin();
  loadCal();
  loadProfiles();
  if (cal.valid) {
    // Seed baseline at normalised white
    baseR = 255; baseG = 255; baseB = 255;
  }
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

  // ── RFID ───────────────────────────────────────────────────────────────────
  {
    static bool tagWasPresent = false;
    byte buf[2]; byte bsz = sizeof(buf);
    if (rfid.PICC_WakeupA(buf, &bsz) == MFRC522::STATUS_OK) {
      if (rfid.PICC_ReadCardSerial()) {
        String uid = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
          if (i) uid += ':';
          if (rfid.uid.uidByte[i] < 0x10) uid += '0';
          uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase();
        if (uid != lastUID) { lastUID = uid; DBG("RFID: "); DBGLN(lastUID); }
        tagWasPresent = true;
        rfid.PCD_StopCrypto1();
      }
    } else {
      tagWasPresent = false;
    }
  }

  // ── Color sensor ────────────────────────────────────────────────────────────────
  {
    static unsigned long lastPoll = 0;
    if (tcsOK && millis() - lastPoll >= 3) {
      lastPoll = millis();
      tcs.getRawData(&liveR, &liveG, &liveB, &liveC);
      updateNorm();  // always recompute normalised values

      float dR = fabsf((float)normR - baseR);
      float dG = fabsf((float)normG - baseG);
      float dB = fabsf((float)normB - baseB);
      float maxDev = max(dR, max(dG, dB));

      if (!inEvent) {
        if (maxDev > DETECT_THRESH) {
          inEvent    = true;
          peakDev    = maxDev;
          peakR = normR; peakG = normG; peakB = normB;
          settleStart = 0;
          DBG("Event start dev="); DBGLN(maxDev);
        } else {
          // Adapt baseline slowly when idle
          baseR += EMA_ALPHA * ((float)normR - baseR);
          baseG += EMA_ALPHA * ((float)normG - baseG);
          baseB += EMA_ALPHA * ((float)normB - baseB);
        }
      } else {
        // Track sample with greatest deviation during the event
        if (maxDev > peakDev) {
          peakDev = maxDev;
          peakR = normR; peakG = normG; peakB = normB;
        }
        if (maxDev <= DETECT_THRESH) {
          if (settleStart == 0) settleStart = millis();
          if (millis() - settleStart >= SETTLE_MS) {
            // Commit: classify peak against trained profiles
            detR = peakR; detG = peakG; detB = peakB;
            int match = matchProfile(peakR, peakG, peakB);
            if (match >= 0) {
              detName  = String(profiles[match].name);
              detDist  = profileDist(profiles[match], peakR, peakG, peakB);
              detMatch = true;
              DBG("Match: "); DBG(detName); DBG(" dist="); DBGLN(detDist);
            } else {
              detName  = "Unknown";
              detDist  = 0;
              detMatch = false;
              DBG("No match for R="); DBG(peakR); DBG(" G="); DBG(peakG); DBG(" B="); DBGLN(peakB);
            }
            inEvent     = false;
            settleStart = 0;
            peakDev     = 0;
            baseR = normR; baseG = normG; baseB = normB;
          }
        } else {
          settleStart = 0;
        }
      }
    }
  }
}
