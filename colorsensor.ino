// =============================================================================
//  colorsensor.ino  —  ESP32-C3 | MFRC522 (SPI) + TCS3472 (I2C) | WiFi UI
//                      + EM4100 125kHz RFID via RDM630 Manchester on GPIO 0
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
#include <Secrets.h>
#include "html.h"

// ── Debug ───────────────────────────────────────────────────────────────────────
#define DEBUG 1
#if DEBUG
  #define DBG(x)   Serial.print(x)
  #define DBGLN(x) Serial.println(x)
#else
  #define DBG(x)
  #define DBGLN(x)
#endif

#define HOSTNAME        "colorsensor"
#define WIFI_TIMEOUT_MS  12000UL
#define RECONNECT_MS      5000UL

// ── Pins ───────────────────────────────────────────────────────────────────────
#define RFID_SS   7
#define RFID_SCK  8
#define RFID_MISO 9
#define RFID_MOSI 10
#define RFID_RST  1
#define COLOR_SDA 4
#define COLOR_SCL 6

// ── Constants & Structs (must precede all function definitions) ────────────────
#define MAX_PROFILES 16
#define NAME_LEN     20
#define SAMP_BUF     64

struct ColorProfile {
  char    name[NAME_LEN];
  uint8_t r, g, b;
  bool    used = false;
};

struct CalData {
  uint16_t dR, dG, dB;
  uint16_t wR, wG, wB;
  bool valid = false;
};

struct DetectSettings {
  float    trigRatio;   // default 0.85
  uint16_t minEventMs;  // default 20
  uint16_t maxEventMs;  // default 2000
  float    matchDist;   // default 120.0
  float    emaAlpha;    // default 0.05
};

// ── EM4100 125kHz Manchester decoder (RDM630 DEMOD_OUT on GPIO 0) ──────────────
// EM4100 frame = 64 bits:
//   9  header bits (all 1)
//   10 rows x 5 bits (4 data + 1 even row-parity)
//   4  column-parity bits
//   1  stop bit (0)
//
// Oscope-confirmed timing for this RDM630 unit:
//   Half-bit periods cluster tightly at 200-300 us
//   Full-bit periods cluster tightly at 450-540 us
//   Dead zone 350-450 us: no real pulses, occasional ISR jitter lands here.
//
// Polarity: DEMOD_OUT idles HIGH, goes LOW during transmission.
//   prev = level (not level^1) gives correct Manchester sense.
//
// Jitter tolerance: Manchester errors from jitter-stretched pulses are
//   tolerated by removing the early-exit 'errs > 0' gate. Corrupt bits
//   become 0xFF and are caught by em4100Validate's 'if (d > 1) return false'
//   check. Clean frames pass parity normally; frames with too many errors
//   still fail validate cleanly. Confirmed working via standalone debug sketch.

#define EM4100_PIN       0
#define EM_HALF_MIN_US   120
#define EM_HALF_MAX_US   350
// 350-450 us dead zone: skip silently, buffer preserved
#define EM_FULL_MIN_US   450
#define EM_FULL_MAX_US   700
#define EM_IDLE_US       1500
#define EM_FRAME_BITS    64
#define EM_HEADER_BITS   9
#define EM_MAX_HALFBUF   352
#define EM_READY_THRESH  (EM_FRAME_BITS * 4)

volatile uint32_t em_lastEdge   = 0;
volatile uint8_t  em_halfBuf[EM_MAX_HALFBUF];
volatile uint16_t em_halfCount  = 0;
volatile bool     em_frameReady = false;
volatile uint32_t em_dbgEdges   = 0;

String last125ID = "None";

void IRAM_ATTR em4100_isr() {
  em_dbgEdges++;
  uint32_t now   = micros();
  uint32_t width = now - em_lastEdge;
  em_lastEdge    = now;

  if (width > EM_IDLE_US) { em_halfCount = 0; return; }
  if (em_frameReady) return;

  // Correct polarity: store the level that was present BEFORE this edge
  uint8_t level = (uint8_t)digitalRead(EM4100_PIN);
  uint8_t prev  = level;  // NOTE: not level^1 - confirmed correct by debug sketch

  if (width >= EM_HALF_MIN_US && width <= EM_HALF_MAX_US) {
    if (em_halfCount < EM_MAX_HALFBUF)
      em_halfBuf[em_halfCount++] = prev;
  } else if (width >= EM_FULL_MIN_US && width <= EM_FULL_MAX_US) {
    if (em_halfCount + 1 < EM_MAX_HALFBUF) {
      em_halfBuf[em_halfCount++] = prev;
      em_halfBuf[em_halfCount++] = prev;
    }
  }
  // Dead zone or out-of-range: skip silently, preserve buffer

  if (em_halfCount >= EM_READY_THRESH)
    em_frameReady = true;
}

static uint8_t manchesterDecode(const uint8_t *hp, uint16_t len,
                                uint16_t offset, uint8_t *bits, uint8_t nBits) {
  uint8_t errors = 0;
  for (uint8_t i = 0; i < nBits; i++) {
    uint16_t idx = offset + i * 2;
    if (idx + 1 >= len) { errors++; continue; }
    uint8_t a = hp[idx], b = hp[idx + 1];
    if      (a == 0 && b == 1) bits[i] = 1;
    else if (a == 1 && b == 0) bits[i] = 0;
    else                       { bits[i] = 0xFF; errors++; }
  }
  return errors;
}

static bool em4100Validate(const uint8_t *bits, char *hexOut) {
  // Header: 9 ones
  for (uint8_t i = 0; i < EM_HEADER_BITS; i++)
    if (bits[i] != 1) return false;

  uint8_t cardBits[40];
  uint8_t colAcc[4] = {0, 0, 0, 0};

  for (uint8_t row = 0; row < 10; row++) {
    uint8_t base = EM_HEADER_BITS + row * 5;
    uint8_t d0 = bits[base], d1 = bits[base+1],
            d2 = bits[base+2], d3 = bits[base+3],
            rp = bits[base+4];
    // 0xFF from Manchester errors will fail this check
    if (d0 > 1 || d1 > 1 || d2 > 1 || d3 > 1 || rp > 1) return false;
    if (((d0 ^ d1 ^ d2 ^ d3) & 1) != rp) return false;
    cardBits[row*4]   = d0; cardBits[row*4+1] = d1;
    cardBits[row*4+2] = d2; cardBits[row*4+3] = d3;
    colAcc[0] ^= d0; colAcc[1] ^= d1;
    colAcc[2] ^= d2; colAcc[3] ^= d3;
  }

  uint8_t cpBase = EM_HEADER_BITS + 50;
  for (uint8_t c = 0; c < 4; c++)
    if (bits[cpBase + c] != colAcc[c]) return false;

  if (bits[63] != 0) return false;

  uint32_t hi = 0, lo = 0;
  for (uint8_t i = 0; i < 8;  i++) hi = (hi << 1) | cardBits[i];
  for (uint8_t i = 8; i < 40; i++) lo = (lo << 1) | cardBits[i];
  snprintf(hexOut, 11, "%02X%08X", (unsigned)hi, (unsigned)lo);
  return true;
}

void em4100Process() {
  if (!em_frameReady) return;

  uint8_t  hp[EM_MAX_HALFBUF];
  uint16_t hpLen;
  noInterrupts();
  hpLen = em_halfCount;
  memcpy(hp, (const void*)em_halfBuf, hpLen);
  em_halfCount  = 0;
  em_frameReady = false;
  interrupts();

  if (hpLen < (uint16_t)(EM_FRAME_BITS * 2)) return;

  uint8_t bits[EM_FRAME_BITS];
  char    hexOut[11];

  uint16_t maxOffset = hpLen - (uint16_t)(EM_FRAME_BITS * 2);
  for (uint16_t offset = 0; offset <= maxOffset; offset += 2) {
    // Decode regardless of Manchester error count:
    // validate() will reject corrupt bits (0xFF) via the >1 check
    manchesterDecode(hp, hpLen, offset, bits, EM_FRAME_BITS);
    if (em4100Validate(bits, hexOut)) {
      String newID = String(hexOut);
      if (newID != last125ID) {
        last125ID = newID;
        DBG("[125k] Card: "); DBGLN(last125ID);
      }
      return;
    }
  }
}

// ── All globals ─────────────────────────────────────────────────────────────────
Preferences       prefs;
WebServer         server(80);
DNSServer         dns;
MFRC522           rfid(RFID_SS, RFID_RST);
Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_2_4MS, TCS34725_GAIN_16X);

bool   apMode = false;
bool   tcsOK  = false;
String lastUID = "None";

uint16_t liveR = 0, liveG = 0, liveB = 0, liveC = 0;
uint8_t  normR = 0, normG = 0, normB = 0;

float    baseC = 1000;

uint8_t  detR = 0, detG = 0, detB = 0;
String   detName   = "—";
float    detDist   = 0;
float    detConf   = 0;
bool     detMatch  = false;
uint32_t detEventMs = 0;

enum EventState { IDLE, ACTIVE, SETTLING };
EventState evState   = IDLE;
uint32_t   evStartMs = 0;
uint32_t   evEndMs   = 0;
float      accumR = 0, accumG = 0, accumB = 0;
uint16_t   accumN = 0;
uint8_t    sampR[SAMP_BUF], sampG[SAMP_BUF], sampB[SAMP_BUF];
uint8_t    sampHead = 0;

float lastEventDur = 0;
float lastAvgR = 0, lastAvgG = 0, lastAvgB = 0;

CalData        cal;
ColorProfile   profiles[MAX_PROFILES];
uint8_t        numProfiles = 0;
DetectSettings ds;

// ── Settings NVS ────────────────────────────────────────────────────────────────
void loadSettings() {
  prefs.begin("dset", true);
  ds.trigRatio  = prefs.getFloat("trigRatio",  0.85f);
  ds.minEventMs = prefs.getUShort("minEvMs",   20);
  ds.maxEventMs = prefs.getUShort("maxEvMs",   2000);
  ds.matchDist  = prefs.getFloat("matchDist",  120.0f);
  ds.emaAlpha   = prefs.getFloat("emaAlpha",   0.05f);
  prefs.end();
}

void saveSettings() {
  prefs.begin("dset", false);
  prefs.putFloat("trigRatio",  ds.trigRatio);
  prefs.putUShort("minEvMs",   ds.minEventMs);
  prefs.putUShort("maxEvMs",   ds.maxEventMs);
  prefs.putFloat("matchDist",  ds.matchDist);
  prefs.putFloat("emaAlpha",   ds.emaAlpha);
  prefs.end();
}

// ── Calibration NVS ────────────────────────────────────────────────────────────
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

// ── Profile NVS ─────────────────────────────────────────────────────────────────
void loadProfiles() {
  prefs.begin("profiles", true);
  numProfiles = prefs.getUChar("count", 0);
  for (uint8_t i = 0; i < numProfiles && i < MAX_PROFILES; i++) {
    String key = "p" + String(i);
    prefs.getBytes(key.c_str(), &profiles[i], sizeof(ColorProfile));
    profiles[i].used = true;
  }
  prefs.end();
  DBG("Profiles loaded: "); DBGLN(numProfiles);
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

float profileDist(const ColorProfile &p, uint8_t r, uint8_t g, uint8_t b) {
  float dr = (float)p.r - r;
  float dg = (float)p.g - g;
  float db = (float)p.b - b;
  return sqrtf(dr*dr + dg*dg + db*db);
}

int matchProfile(uint8_t r, uint8_t g, uint8_t b) {
  int   best  = -1;
  float bestD = ds.matchDist;
  for (uint8_t i = 0; i < numProfiles; i++) {
    float d = profileDist(profiles[i], r, g, b);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

// ── WiFi ───────────────────────────────────────────────────────────────────────
bool connectWiFi(const String &ssid, const String &psk) {
  WiFi.mode(WIFI_STA); WiFi.setHostname(HOSTNAME); WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(ssid.c_str(), psk.c_str());
  DBG("Connecting to "); DBGLN(ssid);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) { delay(250); DBG('.'); }
  DBGLN();
  if (WiFi.status() == WL_CONNECTED) { DBG("IP: "); DBGLN(WiFi.localIP()); return true; }
  DBGLN("WiFi failed"); return false;
}
void startCaptivePortal() {
  apMode = true; WiFi.mode(WIFI_AP); WiFi.softAP(HOSTNAME);
  dns.start(53, "*", WiFi.softAPIP()); DBG("AP IP: "); DBGLN(WiFi.softAPIP());
}
void startNetServices() {
  MDNS.end(); MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() { DBGLN("OTA start"); });
  ArduinoOTA.onError([](ota_error_t e) { DBG("OTA err "); DBGLN(e); });
  ArduinoOTA.begin();
  DBGLN("mDNS+OTA ready → " HOSTNAME ".local");
}

// ── Web routes ────────────────────────────────────────────────────────────────
void handleRoot() { server.send_P(200, "text/html", apMode ? WIFI_HTML : INDEX_HTML); }

void handleData() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", detR, detG, detB);
  String j = "{\"r\":" + String(detR) + ",\"g\":" + String(detG) + ",\"b\":" + String(detB) +
    ",\"hex\":\"" + hex + "\",\"name\":\"" + detName + "\"" +
    ",\"dist\":"   + String(detDist, 1) +
    ",\"conf\":"   + String(detConf, 1) +
    ",\"match\":"  + (detMatch ? "true" : "false") +
    ",\"uid\":\""  + lastUID   + "\"" +
    ",\"id125\":\"" + last125ID + "\"" +
    ",\"calOK\":"  + (cal.valid ? "true" : "false") +
    ",\"event\":"  + (evState != IDLE ? "true" : "false") +
    ",\"evDur\":"  + String(detEventMs) + "}";
  server.send(200, "application/json", j);
}

void handleLiveData() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", normR, normG, normB);
  float cRatio = baseC > 0 ? (float)liveC / baseC : 1.0f;
  String j = "{\"lr\":" + String(liveR) + ",\"lg\":" + String(liveG) +
    ",\"lb\":" + String(liveB) + ",\"lc\":" + String(liveC) +
    ",\"nr\":" + String(normR) + ",\"ng\":" + String(normG) + ",\"nb\":" + String(normB) +
    ",\"hex\":\"" + hex + "\"" +
    ",\"baseC\":"    + String((int)baseC) +
    ",\"cRatio\":"   + String(cRatio, 3) +
    ",\"trigRatio\":" + String(ds.trigRatio, 3) +
    ",\"event\":"    + (evState != IDLE ? "true" : "false") +
    ",\"avgR\":"  + String((int)lastAvgR) + ",\"avgG\":" + String((int)lastAvgG) + ",\"avgB\":" + String((int)lastAvgB) +
    ",\"lastDur\":" + String((int)lastEventDur) + "}";
  server.send(200, "application/json", j);
}

void handleDebug125() {
  uint32_t edges;
  uint16_t hcount;
  bool     frdy;
  noInterrupts();
  edges  = em_dbgEdges;
  hcount = em_halfCount;
  frdy   = em_frameReady;
  interrupts();
  String j = "{\"edges\":"     + String(edges) +
             ",\"halfCount\":" + String(hcount) +
             ",\"frameReady\":" + (frdy ? "true" : "false") +
             ",\"id125\":\""   + last125ID + "\"}";
  server.send(200, "application/json", j);
}

void handleProfilesList() {
  String j = "[";
  for (uint8_t i = 0; i < numProfiles; i++) {
    char hex[8]; snprintf(hex, sizeof(hex), "#%02X%02X%02X", profiles[i].r, profiles[i].g, profiles[i].b);
    if (i) j += ",";
    j += "{\"i\":" + String(i) + ",\"name\":\"" + String(profiles[i].name) + "\"" +
         ",\"r\":" + String(profiles[i].r) + ",\"g\":" + String(profiles[i].g) + ",\"b\":" + String(profiles[i].b) +
         ",\"hex\":\"" + hex + "\"}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

void handleProfileTrain() {
  if (!server.hasArg("name") || server.arg("name").length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Missing name\"}"); return;
  }
  if (numProfiles >= MAX_PROFILES) {
    server.send(400, "application/json", "{\"error\":\"Max profiles reached\"}"); return;
  }
  String name = server.arg("name"); name.trim();
  ColorProfile &p = profiles[numProfiles];
  strncpy(p.name, name.c_str(), NAME_LEN - 1); p.name[NAME_LEN - 1] = 0;
  p.r = normR; p.g = normG; p.b = normB; p.used = true;
  numProfiles++; saveProfiles();
  char hex[8]; snprintf(hex, sizeof(hex), "#%02X%02X%02X", p.r, p.g, p.b);
  DBG("Trained "); DBG(p.name); DBG(" R="); DBG(p.r); DBG(" G="); DBG(p.g); DBG(" B="); DBGLN(p.b);
  server.send(200, "application/json",
    "{\"ok\":true,\"name\":\"" + String(p.name) + "\",\"hex\":\"" + hex + "\"}");
}

void handleProfileDelete() {
  if (!server.hasArg("i")) { server.send(400, "application/json", "{\"error\":\"Missing i\"}"); return; }
  uint8_t idx = (uint8_t)server.arg("i").toInt();
  if (idx >= numProfiles) { server.send(400, "application/json", "{\"error\":\"Bad index\"}"); return; }
  for (uint8_t i = idx; i < numProfiles - 1; i++) profiles[i] = profiles[i + 1];
  numProfiles--; saveProfiles();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetSettings() {
  String j = "{\"trigRatio\":"  + String(ds.trigRatio, 3) +
    ",\"minEventMs\":" + String(ds.minEventMs) +
    ",\"maxEventMs\":" + String(ds.maxEventMs) +
    ",\"matchDist\":"  + String(ds.matchDist, 1) +
    ",\"emaAlpha\":"   + String(ds.emaAlpha, 3) + "}";
  server.send(200, "application/json", j);
}

void handleSetSettings() {
  bool changed = false;
  if (server.hasArg("trigRatio"))  { ds.trigRatio  = constrain(server.arg("trigRatio").toFloat(),  0.50f, 0.99f); changed = true; }
  if (server.hasArg("minEventMs")) { ds.minEventMs = (uint16_t)constrain(server.arg("minEventMs").toInt(), 5,   500);  changed = true; }
  if (server.hasArg("maxEventMs")) { ds.maxEventMs = (uint16_t)constrain(server.arg("maxEventMs").toInt(), 100, 10000); changed = true; }
  if (server.hasArg("matchDist"))  { ds.matchDist  = constrain(server.arg("matchDist").toFloat(),  10.0f, 441.0f); changed = true; }
  if (server.hasArg("emaAlpha"))   { ds.emaAlpha   = constrain(server.arg("emaAlpha").toFloat(),   0.01f, 0.30f);  changed = true; }
  if (changed) saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetWifi() {
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing ssid"); return; }
  prefs.begin("wifi", false);
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("psk",  server.arg("psk"));
  prefs.end();
  server.send(200, "text/html", "<meta http-equiv='refresh' content='3;url=/'><p>Rebooting…</p>");
  delay(1500); ESP.restart();
}

void handleCalBlack() {
  cal.dR = liveR; cal.dG = liveG; cal.dB = liveB; saveCal();
  DBG("Cal black R="); DBG(cal.dR); DBG(" G="); DBG(cal.dG); DBG(" B="); DBGLN(cal.dB);
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleCalWhite() {
  cal.wR = liveR; cal.wG = liveG; cal.wB = liveB; cal.valid = true; saveCal();
  updateNorm();
  baseC = liveC;
  DBG("Cal white R="); DBG(cal.wR); DBG(" G="); DBG(cal.wG); DBG(" B="); DBGLN(cal.wB);
  DBG("  baseC="); DBGLN(baseC);
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleCalReset() {
  prefs.begin("cal", false); prefs.clear(); prefs.end();
  cal = {0, 0, 0, 4095, 4095, 4095, false};
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupServer() {
  server.on("/",                 HTTP_GET,  handleRoot);
  server.on("/data",             HTTP_GET,  handleData);
  server.on("/livedata",         HTTP_GET,  handleLiveData);
  server.on("/debug125",         HTTP_GET,  handleDebug125);
  server.on("/profiles",         HTTP_GET,  handleProfilesList);
  server.on("/profiles/train",   HTTP_POST, handleProfileTrain);
  server.on("/profiles/delete",  HTTP_POST, handleProfileDelete);
  server.on("/settings",         HTTP_GET,  handleGetSettings);
  server.on("/settings",         HTTP_POST, handleSetSettings);
  server.on("/setwifi",          HTTP_POST, handleSetWifi);
  server.on("/cal/black",        HTTP_POST, handleCalBlack);
  server.on("/cal/white",        HTTP_POST, handleCalWhite);
  server.on("/cal/reset",        HTTP_POST, handleCalReset);
  server.onNotFound([]() {
    server.sendHeader("Location",
      String("http://") + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "/");
    server.send(302);
  });
  server.begin();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
#if DEBUG
  Serial.begin(115200); delay(400);
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
  rfid.PCD_Init(); rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  #if DEBUG
    DBG("RFID gain: 0x"); DBGLN(String(rfid.PCD_ReadRegister(rfid.RFCfgReg), HEX));
  #endif
  DBGLN("RFID ready");

  Wire.begin(COLOR_SDA, COLOR_SCL);
  tcsOK = tcs.begin();
  loadCal(); loadProfiles(); loadSettings();

  if (cal.valid) baseC = 2000;
  DBGLN(tcsOK ? "TCS3472 ready (2.4ms/16x)" : "TCS3472 NOT found");
  DBG("Settings: trigRatio="); DBG(ds.trigRatio);
  DBG(" minMs="); DBG(ds.minEventMs);
  DBG(" maxMs="); DBG(ds.maxEventMs);
  DBG(" matchDist="); DBG(ds.matchDist);
  DBG(" ema="); DBGLN(ds.emaAlpha);

  pinMode(EM4100_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(EM4100_PIN), em4100_isr, CHANGE);
  DBGLN("EM4100 decoder ready → GPIO 0");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  static unsigned long lastReconnect = 0;
  if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastReconnect > RECONNECT_MS) {
    lastReconnect = millis(); DBGLN("WiFi lost — reconnecting…");
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", MYSSID);
    String psk  = prefs.getString("psk",  MYPSK);
    prefs.end();
    if (connectWiFi(ssid, psk)) startNetServices();
  }
  if (apMode) dns.processNextRequest(); else ArduinoOTA.handle();
  server.handleClient();

  em4100Process();

  // MFRC522 13.56MHz RFID
  {
    static bool tagPresent = false;
    byte buf[2]; byte bsz = sizeof(buf);
    if (rfid.PICC_WakeupA(buf, &bsz) == MFRC522::STATUS_OK) {
      if (rfid.PICC_ReadCardSerial()) {
        String uid = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
          if (i) uid += ':'; if (rfid.uid.uidByte[i] < 0x10) uid += '0';
          uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase();
        if (uid != lastUID) { lastUID = uid; DBG("RFID: "); DBGLN(lastUID); }
        tagPresent = true; rfid.PCD_StopCrypto1();
      }
    } else { tagPresent = false; }
  }

  // Color sensor
  {
    static unsigned long lastPoll = 0;
    if (!tcsOK || millis() - lastPoll < 3) goto done;
    lastPoll = millis();
    tcs.getRawData(&liveR, &liveG, &liveB, &liveC);
    updateNorm();

    switch (evState) {

      case IDLE: {
        baseC += ds.emaAlpha * ((float)liveC - baseC);
        float trigThresh = baseC * ds.trigRatio;
        if ((float)liveC < trigThresh && baseC > 50) {
          evState   = ACTIVE;
          evStartMs = millis();
          accumR = accumG = accumB = 0;
          accumN = 0; sampHead = 0;
          DBG("[EVT] start C="); DBG(liveC); DBG(" baseC="); DBG((int)baseC);
          DBG(" thresh="); DBGLN((int)trigThresh);
        }
        break;
      }

      case ACTIVE: {
        uint32_t dur = millis() - evStartMs;
        accumR += normR; accumG += normG; accumB += normB; accumN++;
        uint8_t si = sampHead % SAMP_BUF;
        sampR[si] = normR; sampG[si] = normG; sampB[si] = normB; sampHead++;
        float trigThresh = baseC * ds.trigRatio;
        bool cleared = (float)liveC >= trigThresh;
        if (cleared) { evEndMs = millis(); evState = SETTLING; DBG("[EVT] settling dur="); DBGLN(dur); }
        if (dur > ds.maxEventMs) {
          evState = IDLE; accumN = 0;
          DBG("[EVT] aborted (too long) dur="); DBGLN(dur);
          baseC = liveC;
        }
        break;
      }

      case SETTLING: {
        uint32_t dur = evEndMs - evStartMs;
        lastEventDur = dur;
        if (dur < ds.minEventMs || accumN == 0) {
          DBG("[EVT] discarded noise dur="); DBGLN(dur);
          evState = IDLE; baseC = liveC; break;
        }
        uint8_t avgR = (uint8_t)(accumR / accumN);
        uint8_t avgG = (uint8_t)(accumG / accumN);
        uint8_t avgB = (uint8_t)(accumB / accumN);
        lastAvgR = avgR; lastAvgG = avgG; lastAvgB = avgB;
        detR = avgR; detG = avgG; detB = avgB;
        detEventMs = dur;
        int match = matchProfile(avgR, avgG, avgB);
        if (match >= 0) {
          detName  = String(profiles[match].name);
          detDist  = profileDist(profiles[match], avgR, avgG, avgB);
          detConf  = max(0.0f, (1.0f - detDist / ds.matchDist) * 100.0f);
          detMatch = true;
          DBG("[EVT] MATCH="); DBG(detName); DBG(" dist="); DBG(detDist); DBG(" conf="); DBG(detConf); DBG("%");
        } else {
          detName  = "Unknown"; detDist = ds.matchDist; detConf = 0; detMatch = false;
          DBG("[EVT] no match");
        }
        DBG(" avgRGB="); DBG(avgR); DBG("/"); DBG(avgG); DBG("/"); DBG(avgB);
        DBG(" dur="); DBG(dur); DBG("ms samples="); DBGLN(accumN);
        evState = IDLE;
        baseC   = liveC;
        break;
      }
    }
  }
  done:;
}
