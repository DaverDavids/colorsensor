// =============================================================================
//  em4100_debug.ino  —  ESP32-C3 standalone EM4100 125kHz decoder debug
//
//  NOTHING else running: no WiFi, no SPI, no I2C, no web server.
//  Goal: confirm whether tag reads succeed when there is zero ISR interference.
//
//  Wiring: RDM630 DEMOD_OUT -> GPIO 0
//          RDM630 VCC       -> 5V
//          RDM630 GND       -> GND
//          SHD pin          -> GND (always active)
// =============================================================================

#include <Arduino.h>

// EM4100 frame = 64 bits:
//   9  header bits (all 1)
//   10 rows x 5 bits (4 data + 1 even row-parity)
//   4  column-parity bits
//   1  stop bit (0)
//
// Oscope-confirmed timing for this RDM630:
//   Half-bit : ~200-300 us
//   Full-bit : ~450-540 us
//   Dead gap : ~350-450 us (no real pulses here)

#define EM4100_PIN      0
#define EM_HALF_MIN_US  120
#define EM_HALF_MAX_US  350
#define EM_FULL_MIN_US  450
#define EM_FULL_MAX_US  700
#define EM_IDLE_US      1500
#define EM_FRAME_BITS   64
#define EM_HEADER_BITS  9
#define EM_MAX_HALFBUF  352
#define EM_READY_THRESH (EM_FRAME_BITS * 4)

volatile uint32_t em_lastEdge  = 0;
volatile uint8_t  em_halfBuf[EM_MAX_HALFBUF];
volatile uint16_t em_halfCount = 0;
volatile bool     em_frameReady = false;
volatile uint32_t em_dbgEdges  = 0;
volatile uint32_t em_dbgSkips  = 0;  // dead-zone skips
volatile uint32_t em_dbgResets = 0;  // idle resets

void IRAM_ATTR em4100_isr() {
  em_dbgEdges++;
  uint32_t now   = micros();
  uint32_t width = now - em_lastEdge;
  em_lastEdge    = now;

  if (width > EM_IDLE_US) { em_halfCount = 0; em_dbgResets++; return; }
  if (em_frameReady) return;

  uint8_t level = (uint8_t)digitalRead(EM4100_PIN);
  uint8_t prev  = level ^ 1;

  if (width >= EM_HALF_MIN_US && width <= EM_HALF_MAX_US) {
    if (em_halfCount < EM_MAX_HALFBUF)
      em_halfBuf[em_halfCount++] = prev;
  } else if (width >= EM_FULL_MIN_US && width <= EM_FULL_MAX_US) {
    if (em_halfCount + 1 < EM_MAX_HALFBUF) {
      em_halfBuf[em_halfCount++] = prev;
      em_halfBuf[em_halfCount++] = prev;
    }
  } else {
    // Dead zone or out-of-range: skip without resetting
    em_dbgSkips++;
    return;
  }

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
  for (uint8_t i = 0; i < EM_HEADER_BITS; i++)
    if (bits[i] != 1) return false;

  uint8_t cardBits[40];
  uint8_t colAcc[4] = {0, 0, 0, 0};

  for (uint8_t row = 0; row < 10; row++) {
    uint8_t base = EM_HEADER_BITS + row * 5;
    uint8_t d0 = bits[base],   d1 = bits[base+1],
            d2 = bits[base+2], d3 = bits[base+3],
            rp = bits[base+4];
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
  uint32_t skips, resets;
  noInterrupts();
  hpLen  = em_halfCount;
  memcpy(hp, (const void*)em_halfBuf, hpLen);
  skips  = em_dbgSkips;
  resets = em_dbgResets;
  em_halfCount   = 0;
  em_frameReady  = false;
  em_dbgSkips    = 0;
  em_dbgResets   = 0;
  interrupts();

  Serial.print("[buf] hpLen="); Serial.print(hpLen);
  Serial.print(" skips=");  Serial.print(skips);
  Serial.print(" resets="); Serial.println(resets);

  if (hpLen < (uint16_t)(EM_FRAME_BITS * 2)) {
    Serial.println("  too short");
    return;
  }

  uint8_t bits[EM_FRAME_BITS];
  char    hexOut[11];

  uint16_t maxOffset = hpLen - (uint16_t)(EM_FRAME_BITS * 2);
  for (uint16_t offset = 0; offset <= maxOffset; offset += 2) {
    uint8_t errs = manchesterDecode(hp, hpLen, offset, bits, EM_FRAME_BITS);
    if (errs > 0) continue;
    if (em4100Validate(bits, hexOut)) {
      Serial.print("  >> CARD: "); Serial.print(hexOut);
      Serial.print(" (offset="); Serial.print(offset); Serial.println(")");
      return;
    }
  }

  // Failed: dump full header scan across all even offsets for analysis
  Serial.println("  FAIL - header scan across all offsets:");
  // Print every offset's first 9 bits on one line so we can see the pattern
  for (uint16_t offset = 0; offset <= maxOffset; offset += 2) {
    manchesterDecode(hp, hpLen, offset, bits, 9);
    bool allClean = true;
    for (uint8_t i = 0; i < 9; i++) if (bits[i] == 0xFF) { allClean = false; break; }
    // Only print offsets with zero X errors in first 9 bits
    if (allClean) {
      Serial.print("    off="); Serial.print(offset); Serial.print(" hdr=");
      for (uint8_t i = 0; i < 9; i++) Serial.print(bits[i]);
      // Also decode full frame and show first failure reason
      uint8_t errs2 = manchesterDecode(hp, hpLen, offset, bits, EM_FRAME_BITS);
      if (errs2 == 0) {
        // Check why validate failed
        bool hdrOK = true;
        for (uint8_t i = 0; i < EM_HEADER_BITS; i++) if (bits[i] != 1) { hdrOK = false; break; }
        if (!hdrOK) { Serial.println(" [bad header]"); continue; }
        // Check rows
        bool rowOK = true;
        for (uint8_t row = 0; row < 10 && rowOK; row++) {
          uint8_t base = EM_HEADER_BITS + row * 5;
          uint8_t d0=bits[base],d1=bits[base+1],d2=bits[base+2],d3=bits[base+3],rp=bits[base+4];
          if (((d0^d1^d2^d3)&1) != rp) {
            Serial.print(" [row "); Serial.print(row); Serial.print(" parity fail]");
            rowOK = false;
          }
        }
        if (rowOK) Serial.println(" [col parity fail]");
        else Serial.println();
      } else {
        Serial.print(" [X errors in frame: "); Serial.print(errs2); Serial.println("]");
      }
    }
  }
  // If no clean offsets at all, say so
  bool anyClean = false;
  for (uint16_t offset = 0; offset <= maxOffset; offset += 2) {
    manchesterDecode(hp, hpLen, offset, bits, 9);
    bool allClean = true;
    for (uint8_t i = 0; i < 9; i++) if (bits[i] == 0xFF) { allClean = false; break; }
    if (allClean) { anyClean = true; break; }
  }
  if (!anyClean) Serial.println("    (no offset has clean 9-bit header)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n== EM4100 standalone debug ==");
  Serial.println("No WiFi, no SPI, no I2C.");
  Serial.print("Listening on GPIO "); Serial.println(EM4100_PIN);
  Serial.println("Hold tag to reader...");

  pinMode(EM4100_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(EM4100_PIN), em4100_isr, CHANGE);
}

void loop() {
  em4100Process();
  // Nothing else. Yield to let the ISR breathe.
  delay(1);
}
