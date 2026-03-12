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

// ── Training ──────────────────────────────────────────────────────────────────
#define MAX_PROFILES 16
#define NAME_LEN     20
#define SAMP_BUF     64   // max samples to average per event

// ── Structs ───────────────────────────────────────────────────────────────────
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

// ── Tunable detection settings (loaded from NVS, editable via UI) ──────────────
struct DetectSettings {
  // Clear-channel trigger: event starts when liveC drops below
  // (baseC * trigRatio). Range 0.5-0.99. Lower = needs bigger drop to trigger.
  float  trigRatio;   // default 0.85  (C must drop to 85% of baseline)

  // Minimum event duration in ms. Events shorter than this are noise.
  uint16_t minEventMs;  // default 20

  // Maximum event duration in ms. Longer than this = object just sitting there.
  uint16_t maxEventMs;  // default 2000

  // Nearest-neighbour match distance ceiling (0-441 in normalised RGB space).
  // Higher = looser matching. Lower = stricter.
  float matchDist;    // default 120.0

  // EMA drift rate for baseline clear channel (0.01-0.2).
  float emaAlpha;     // default 0.05
};

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

// EMA baseline for clear channel (raw counts)
float    baseC = 1000;

// Last committed detection
uint8_t  detR = 0, detG = 0, detB = 0;
String   detName   = "—";
float    detDist   = 0;
float    detConf   = 0;   // 0-100 confidence
bool     detMatch  = false;
uint32_t detEventMs = 0;  // duration of last event (debug)

// Event state machine
enum EventState { IDLE, ACTIVE, SETTLING };
EventState evState     = IDLE;
uint32_t   evStartMs   = 0;
uint32_t   evEndMs     = 0;
// Running accumulator for averaging during event
float      accumR = 0, accumG = 0, accumB = 0;
uint16_t   accumN = 0;
// Sample ring buffer (for debug / future use)
uint8_t    sampR[SAMP_BUF], sampG[SAMP_BUF], sampB[SAMP_BUF];
uint8_t    sampHead = 0;

// Last event stats for /livedata debug
float    lastEventDur = 0;
float    lastAvgR = 0, lastAvgG = 0, lastAvgB = 0;

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
    ",\"dist\":" + String(detDist, 1) +
    ",\"conf\":" + String(detConf, 1) +
    ",\"match\":" + (detMatch ? "true" : "false") +
    ",\"uid\":\"" + lastUID + "\"" +
    ",\"calOK\":" + (cal.valid ? "true" : "false") +
    ",\"event\":" + (evState != IDLE ? "true" : "false") +
    ",\"evDur\":" + String(detEventMs) + "}";
  server.send(200, "application/json", j);
}

void handleLiveData() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", normR, normG, normB);
  // clear channel ratio vs baseline
  float cRatio = baseC > 0 ? (float)liveC / baseC : 1.0f;
  String j = "{\"lr\":" + String(liveR) + ",\"lg\":" + String(liveG) +
    ",\"lb\":" + String(liveB) + ",\"lc\":" + String(liveC) +
    ",\"nr\":" + String(normR) + ",\"ng\":" + String(normG) + ",\"nb\":" + String(normB) +
    ",\"hex\":\"" + hex + "\"" +
    ",\"baseC\":" + String((int)baseC) +
    ",\"cRatio\":" + String(cRatio, 3) +
    ",\"trigRatio\":" + String(ds.trigRatio, 3) +
    ",\"event\":" + (evState != IDLE ? "true" : "false") +
    ",\"avgR\":" + String((int)lastAvgR) + ",\"avgG\":" + String((int)lastAvgG) + ",\"avgB\":" + String((int)lastAvgB) +
    ",\"lastDur\":" + String((int)lastEventDur) + "}";
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
  String j = "{\"trigRatio\":" + String(ds.trigRatio, 3) +
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
  baseC = liveC;   // seed clear baseline from white cal
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

  // seed baseC: if white cal exists use that C value, else use a live reading
  if (cal.valid) {
    // will be updated properly on first white cal; seed generously
    baseC = 2000;
  }
  DBGLN(tcsOK ? "TCS3472 ready (2.4ms/16x)" : "TCS3472 NOT found");
  DBG("Settings: trigRatio="); DBG(ds.trigRatio);
  DBG(" minMs="); DBG(ds.minEventMs);
  DBG(" maxMs="); DBG(ds.maxEventMs);
  DBG(" matchDist="); DBG(ds.matchDist);
  DBG(" ema="); DBGLN(ds.emaAlpha);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  // WiFi reconnect
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

  // RFID
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
        // Drift baseC slowly toward current clear value
        baseC += ds.emaAlpha * ((float)liveC - baseC);

        // Trigger when clear drops significantly below baseline
        float trigThresh = baseC * ds.trigRatio;
        if ((float)liveC < trigThresh && baseC > 50) {
          evState  = ACTIVE;
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

        // Accumulate normalised samples for averaging
        accumR += normR; accumG += normG; accumB += normB; accumN++;
        // Also store in ring buffer (wraps if very long event)
        uint8_t si = sampHead % SAMP_BUF;
        sampR[si] = normR; sampG[si] = normG; sampB[si] = normB; sampHead++;

        // Check for event end: clear channel recovered above trigger threshold
        float trigThresh = baseC * ds.trigRatio;
        bool cleared = (float)liveC >= trigThresh;

        if (cleared) {
          evEndMs = millis();
          evState = SETTLING;
          DBG("[EVT] settling dur="); DBGLN(dur);
        }

        // Abort if exceeds maxEventMs (object sitting on sensor)
        if (dur > ds.maxEventMs) {
          evState = IDLE;
          accumN  = 0;
          DBG("[EVT] aborted (too long) dur="); DBGLN(dur);
          // resume baseline drift immediately
          baseC = liveC;
        }
        break;
      }

      case SETTLING: {
        uint32_t dur = evEndMs - evStartMs;
        lastEventDur = dur;

        // Discard events shorter than minEventMs (noise/glint)
        if (dur < ds.minEventMs || accumN == 0) {
          DBG("[EVT] discarded noise dur="); DBGLN(dur);
          evState = IDLE;
          baseC = liveC;
          break;
        }

        // Compute average normalised color over entire event
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
          detName  = "Unknown";
          detDist  = ds.matchDist;
          detConf  = 0;
          detMatch = false;
          DBG("[EVT] no match");
        }
        DBG(" avgRGB="); DBG(avgR); DBG("/"); DBG(avgG); DBG("/"); DBG(avgB);
        DBG(" dur="); DBG(dur); DBG("ms samples="); DBGLN(accumN);

        evState = IDLE;
        baseC   = liveC;  // reset baseline to current clear after event
        break;
      }
    }
  }
  done:;
}
