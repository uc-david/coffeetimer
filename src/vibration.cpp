/**
 * @file vibration.cpp
 * @brief Implementierung der Vibrationserkennung (BMI270, FIFO, 400 Hz).
 */
#include "vibration.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>

namespace {

// --- BMI270 (am internen I2C-Bus des CoreS3) ----------------------------------
constexpr uint8_t kAddr = 0x69;
constexpr uint32_t kI2cFreq = 400000;
constexpr uint8_t kRegChipId = 0x00;
constexpr uint8_t kRegFifoLength = 0x24;  ///< 2 Byte, 14 Bit
constexpr uint8_t kRegFifoData = 0x26;
constexpr uint8_t kRegAccConf = 0x40;
constexpr uint8_t kRegAccRange = 0x41;
constexpr uint8_t kRegFifoDowns = 0x45;
constexpr uint8_t kRegFifoConfig0 = 0x48;
constexpr uint8_t kRegFifoConfig1 = 0x49;
constexpr uint8_t kRegPwrCtrl = 0x7D;
constexpr uint8_t kRegCmd = 0x7E;
constexpr uint8_t kChipId = 0x24;
constexpr uint8_t kAccConf400Hz = 0xAA;   ///< Performance-Filter, Bandbreite normal, ODR 400 Hz
constexpr uint8_t kAccRange4g = 0x01;
constexpr float kLsbPerG = 8192.0f;       ///< bei ±4 g
constexpr uint8_t kFifoAccOnly = 0x40;    ///< ohne Header, nur Beschleunigung (6 Byte je Frame)
constexpr uint8_t kFifoFiltered = 0x80;   ///< gefilterte Daten, keine Unterabtastung
constexpr uint8_t kCmdFifoFlush = 0xB0;
constexpr size_t kFrameBytes = 6;

// --- Auswertung ---------------------------------------------------------------
constexpr float kSampleRate = 400.0f;
constexpr size_t kBlockSamples = 40;       ///< 100 ms je Block
constexpr uint32_t kBlockMs = 100;
constexpr uint32_t kReadIntervalMs = 20;
/// Hochpass ~15 Hz: entfernt Schwerkraft und langsame Neigung.
constexpr float kHighPassHz = 15.0f;
constexpr size_t kSteadyBlocks = 8;        ///< Gleichmäßigkeit über ~0,8 s
constexpr size_t kMinRunBlocks = 5;        ///< so viele laute Blöcke bis "aktiv" möglich
constexpr float kSteadyMaxDb = 3.0f;       ///< max. Schwankung beim Einschalten
constexpr float kSteadyHoldDb = 4.5f;      ///< ... während aktiv (Hysterese)
constexpr float kHystFactor = 0.7f;        ///< Schwelle während aktiv
constexpr float kBaselineFall = 0.2f;      ///< leiser: schnell folgen
constexpr float kBaselineRiseDb = 0.05f;   ///< lauter: 0,5 dB/s
/// Länger "aktiv" ist keine Pumpe mehr (neue Dauerquelle) -> als Ruhepegel übernehmen.
constexpr uint32_t kMaxActiveMs = 60000;

Preferences prefs;
vibration::Config cfg;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
vibration::Status st;  ///< geschützt durch mux

// Zustand der Auswertung (nur aus loop())
bool baselineInit = false;
float hpPrevIn[3] = {}, hpPrevOut[3] = {};
bool hpInit = false;
float blockEnergy = 0.0f;
size_t blockCount = 0;
float runLevels[kSteadyBlocks] = {};
size_t runLength = 0;
uint32_t lastReadMs = 0;

bool writeReg(uint8_t reg, uint8_t v) { return M5.In_I2C.writeRegister8(kAddr, reg, v, kI2cFreq); }

bool readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  return M5.In_I2C.readRegister(kAddr, reg, buf, len, kI2cFreq);
}

/// Standardabweichung der letzten Pegel der lauten Phase.
float runSteadiness() {
  const size_t n = runLength < kSteadyBlocks ? runLength : kSteadyBlocks;
  if (n < kMinRunBlocks) return 99.0f;
  float mean = 0.0f;
  for (size_t i = 0; i < n; ++i) mean += runLevels[i];
  mean /= n;
  float var = 0.0f;
  for (size_t i = 0; i < n; ++i) var += (runLevels[i] - mean) * (runLevels[i] - mean);
  return sqrtf(var / n);
}

/// Wertet einen abgeschlossenen 100-ms-Block aus.
void finishBlock(uint32_t now) {
  const float level = 10.0f * log10f(blockEnergy / kBlockSamples + 1e-12f);
  blockEnergy = 0.0f;
  blockCount = 0;

  portENTER_CRITICAL(&mux);
  vibration::Status s = st;
  portEXIT_CRITICAL(&mux);

  if (!baselineInit) {
    s.baselineDb = level;
    baselineInit = true;
  }
  const bool wasActive = s.active;
  const float th = cfg.thresholdDb * (wasActive ? kHystFactor : 1.0f);
  const bool loud = level >= s.baselineDb + th;

  if (loud) {
    if (runLength == 0) s.sinceMs = now - kBlockMs;
    runLevels[runLength % kSteadyBlocks] = level;
    ++runLength;
  } else {
    runLength = 0;
  }
  s.steadiness = runSteadiness();
  s.active = loud && s.steadiness <= (wasActive ? kSteadyHoldDb : kSteadyMaxDb);

  // Dauerquelle (z. B. Kühlschrank auf der Arbeitsplatte) -> neuer Ruhepegel
  if (s.active && now - s.sinceMs > kMaxActiveMs) {
    s.baselineDb = level;
    s.active = false;
    runLength = 0;
  }

  // Ruhepegel nachführen – nie während einer lauten Phase
  if (!loud) {
    if (level < s.baselineDb) {
      s.baselineDb += kBaselineFall * (level - s.baselineDb);
    } else {
      s.baselineDb += fminf(level - s.baselineDb, kBaselineRiseDb);
    }
  }
  s.levelDb = level;

  portENTER_CRITICAL(&mux);
  st = s;
  portEXIT_CRITICAL(&mux);
}

/// Ein Messwert (in g) durch den Hochpass und in den laufenden Block.
void addSample(const float in[3], uint32_t now) {
  // Einpoliger Hochpass: y = a * (y_prev + x - x_prev)
  static const float a = [] {
    const float rc = 1.0f / (2.0f * PI * kHighPassHz);
    const float dt = 1.0f / kSampleRate;
    return rc / (rc + dt);
  }();
  float e = 0.0f;
  for (int k = 0; k < 3; ++k) {
    if (!hpInit) hpPrevIn[k] = in[k];
    const float y = a * (hpPrevOut[k] + in[k] - hpPrevIn[k]);
    hpPrevIn[k] = in[k];
    hpPrevOut[k] = y;
    e += y * y;
  }
  hpInit = true;
  blockEnergy += e;
  if (++blockCount >= kBlockSamples) finishBlock(now);
}

}  // namespace

namespace vibration {

void begin() {
  prefs.begin("vib", false);
  cfg.mode = static_cast<Mode>(constrain(prefs.getUChar("mode", 0), 0, 2));
  cfg.thresholdDb = constrain(prefs.getFloat("thDb", cfg.thresholdDb), 2.0f, 30.0f);

  uint8_t id = 0;
  const bool found = M5.Imu.isEnabled() && readRegs(kRegChipId, &id, 1) && id == kChipId;
  if (!found) {
    Serial.println("[vib] BMI270 nicht gefunden – Vibrationserkennung nicht verfügbar");
    return;
  }
  uint8_t pwr = 0;
  readRegs(kRegPwrCtrl, &pwr, 1);
  const bool ok = writeReg(kRegAccConf, kAccConf400Hz) && writeReg(kRegAccRange, kAccRange4g) &&
                  writeReg(kRegFifoDowns, kFifoFiltered) && writeReg(kRegFifoConfig0, 0x00) &&
                  writeReg(kRegFifoConfig1, kFifoAccOnly) && writeReg(kRegPwrCtrl, pwr | 0x04) &&
                  writeReg(kRegCmd, kCmdFifoFlush);
  portENTER_CRITICAL(&mux);
  st.available = ok;
  portEXIT_CRITICAL(&mux);
  Serial.printf("[vib] BMI270 %s (Modus %s)\n", ok ? "bereit, 400 Hz" : "FEHLER bei der Einrichtung",
                modeName(cfg.mode));
}

void loop() {
  const uint32_t now = millis();
  if (now - lastReadMs < kReadIntervalMs) return;
  lastReadMs = now;
  portENTER_CRITICAL(&mux);
  const bool available = st.available;
  portEXIT_CRITICAL(&mux);
  // Auch im Mikrofon-Modus messen: Werte auf der Webseite helfen beim Einstellen.
  if (!available) return;

  uint8_t lenBuf[2];
  if (!readRegs(kRegFifoLength, lenBuf, 2)) return;
  size_t len = lenBuf[0] | ((lenBuf[1] & 0x3F) << 8);
  len -= len % kFrameBytes;

  // In Stücken lesen (kleiner I2C-Puffer); bei 400 Hz sind es ~48 Byte je 20 ms.
  uint8_t buf[120];
  while (len > 0) {
    const size_t n = len < sizeof(buf) ? len : sizeof(buf);
    if (!readRegs(kRegFifoData, buf, n)) return;
    for (size_t i = 0; i + kFrameBytes <= n; i += kFrameBytes) {
      float g[3];
      for (int k = 0; k < 3; ++k) {
        g[k] = static_cast<int16_t>(buf[i + 2 * k] | (buf[i + 2 * k + 1] << 8)) / kLsbPerG;
      }
      addSample(g, now);
    }
    len -= n;
  }
}

Config config() { return cfg; }

void setConfig(const Config& c) {
  cfg.mode = c.mode <= Mode::Both ? c.mode : Mode::Mic;
  cfg.thresholdDb = constrain(c.thresholdDb, 2.0f, 30.0f);
  prefs.putUChar("mode", static_cast<uint8_t>(cfg.mode));
  prefs.putFloat("thDb", cfg.thresholdDb);
}

Status status() {
  portENTER_CRITICAL(&mux);
  Status s = st;
  portEXIT_CRITICAL(&mux);
  if (cfg.mode == Mode::Mic) s.active = false;
  return s;
}

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Mic:  return "Mikrofon";
    case Mode::Vib:  return "Vibration";
    case Mode::Both: return "Mikrofon+Vibration";
  }
  return "?";
}

}  // namespace vibration
