/**
 * @file settings.cpp
 * @brief Persistenz: Profil-Slots im LittleFS, kleine Werte im NVS.
 */
#include "settings.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

#include <memory>

namespace {

constexpr const char* kNamespace = "coffeetimer";

/// Kennung und Version des Slot-Dateiformats. Bei Änderungen an PumpProfile,
/// NoiseProfile oder DetectorSettings die Version erhöhen.
constexpr uint32_t kSlotMagic = 0x43544D50;  // "CTMP"
constexpr uint32_t kSlotVersion = 5;

/// Binäres Dateiformat eines Slots.
struct SlotFile {
  uint32_t magic;
  uint32_t version;
  ProfileSlot slot;
};

// Alte NVS-Formate (Firmware 1.3/1.4) für die einmalige Übernahme
constexpr uint32_t kLegacyVersion = 4;
struct LegacyNoise {
  uint32_t version;
  NoiseProfile noise;
};

Preferences prefs;
bool fsOk = false;

/// Große Puffer statisch statt auf dem Stack (~9 kB).
SlotFile fileBuf;

String slotPath(uint8_t slot) {
  return "/slot" + String(slot + 1) + ".bin";
}

/**
 * Übernimmt ein Profil aus dem alten NVS-Speicher in Slot 1 und löscht die
 * alten Einträge (geben NVS-Platz frei).
 */
void migrateLegacy() {
  if (!prefs.isKey("profile") && !prefs.isKey("noise")) return;

  // Puffer nur einmalig nötig -> Heap (große Blöcke landen im PSRAM)
  std::unique_ptr<ProfileSlot> slotPtr(new ProfileSlot);
  ProfileSlot& slot = *slotPtr;
  settings::defaultSlot(0, slot);
  bool found = false;

  // PumpProfile v4 = heutiges Format ohne das letzte Feld (micGain).
  const size_t legacyProfileLen = sizeof(uint32_t) + sizeof(PumpProfile) - sizeof(float);
  if (prefs.getBytesLength("profile") == legacyProfileLen) {
    std::unique_ptr<uint8_t[]> rawPtr(new uint8_t[sizeof(uint32_t) + sizeof(PumpProfile)]);
    uint8_t* raw = rawPtr.get();
    prefs.getBytes("profile", raw, legacyProfileLen);
    uint32_t version;
    memcpy(&version, raw, sizeof(version));
    if (version == kLegacyVersion) {
      memcpy(&slot.pump, raw + sizeof(uint32_t), sizeof(PumpProfile) - sizeof(float));
      slot.pump.micGain = cfg::kMicGainDefault;
      found = slot.pump.valid;
    }
  }
  if (found && prefs.getBytesLength("noise") == sizeof(LegacyNoise)) {
    std::unique_ptr<LegacyNoise> lnPtr(new LegacyNoise);
    LegacyNoise& ln = *lnPtr;
    prefs.getBytes("noise", &ln, sizeof(ln));
    if (ln.version == kLegacyVersion) slot.noise = ln.noise;
  }
  if (found) {
    DetectorSettings& s = slot.settings;
    s.similarityThreshold = prefs.getFloat("simTh", s.similarityThreshold);
    s.periodicityThreshold = prefs.getFloat("perTh", s.periodicityThreshold);
    s.levelThreshold = prefs.getFloat("lvlTh", s.levelThreshold);
    s.onDelayMs = prefs.getUShort("onDelay", s.onDelayMs);
    s.offDelayMs = prefs.getUShort("offDelay", s.offDelayMs);
    s.minShotSeconds = prefs.getFloat("minShot", s.minShotSeconds);
    settings::clamp(s);
    if (!settings::slotExists(0) && settings::saveSlot(0, slot)) {
      Serial.println("[settings] altes Profil in Slot 1 übernommen");
    }
  }
  for (const char* key : {"profile", "noise", "simTh", "perTh", "lvlTh", "onDelay", "offDelay", "minShot"}) {
    prefs.remove(key);
  }
}

}  // namespace

namespace settings {

void begin() {
  prefs.begin(kNamespace, false);
  // Partition "spiffs" aus default_16MB.csv; beim ersten Start formatieren.
  fsOk = LittleFS.begin(true, "/littlefs", 5, "spiffs");
  if (!fsOk) Serial.println("[settings] FEHLER: Dateisystem nicht verfügbar");
  migrateLegacy();
}

void defaultSlot(uint8_t slot, ProfileSlot& out) {
  out = ProfileSlot{};
  snprintf(out.name, sizeof(out.name), "Profil %u", (unsigned)(slot + 1));

  // Standardprofil: keine Klangmuster, kein Takt -> erkannt wird jedes
  // gleichmäßige Geräusch deutlich über dem Hintergrund.
  PumpProfile& p = out.pump;
  p.valid = true;
  p.recordings = 0;
  p.templateCount = 0;
  p.periodic = false;
  p.bgRmsDb = cfg::kDefaultBgDb;
  p.pumpRmsDb = cfg::kDefaultPumpDb;
  for (float& b : p.bgBandDb) b = cfg::kDefaultBgDb - 30.0f;
  p.steadyThreshold = cfg::kDefaultSteadyDb;
  p.micGain = cfg::kMicGainDefault;
}

bool slotExists(uint8_t slot) {
  return fsOk && slot < cfg::kProfileSlots && LittleFS.exists(slotPath(slot));
}

bool loadSlot(uint8_t slot, ProfileSlot& out) {
  defaultSlot(slot, out);
  if (!slotExists(slot)) return false;
  File f = LittleFS.open(slotPath(slot), "r");
  if (!f) return false;
  const size_t n = f.read(reinterpret_cast<uint8_t*>(&fileBuf), sizeof(fileBuf));
  f.close();
  if (n != sizeof(fileBuf) || fileBuf.magic != kSlotMagic || fileBuf.version != kSlotVersion) {
    Serial.printf("[settings] Slot %u ungültig (Größe/Version)\n", slot + 1);
    return false;
  }
  out = fileBuf.slot;
  out.name[sizeof(out.name) - 1] = '\0';
  clamp(out.settings);
  return true;
}

bool saveSlot(uint8_t slot, const ProfileSlot& in) {
  if (!fsOk || slot >= cfg::kProfileSlots) return false;
  fileBuf.magic = kSlotMagic;
  fileBuf.version = kSlotVersion;
  fileBuf.slot = in;

  // Erst in eine temporäre Datei schreiben, dann umbenennen: Bei Stromausfall
  // während des Schreibens bleibt die alte Datei erhalten.
  const String path = slotPath(slot);
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t n = f.write(reinterpret_cast<const uint8_t*>(&fileBuf), sizeof(fileBuf));
  f.close();
  if (n != sizeof(fileBuf)) {
    LittleFS.remove(tmp);
    Serial.printf("[settings] FEHLER beim Schreiben von Slot %u\n", slot + 1);
    return false;
  }
  LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) return false;
  return true;
}

bool deleteSlot(uint8_t slot) {
  if (!slotExists(slot)) return true;
  return LittleFS.remove(slotPath(slot));
}

bool encodeSlot(const ProfileSlot& in, String& out) {
  fileBuf.magic = kSlotMagic;
  fileBuf.version = kSlotVersion;
  fileBuf.slot = in;
  size_t len = 0;
  mbedtls_base64_encode(nullptr, 0, &len, reinterpret_cast<const uint8_t*>(&fileBuf), sizeof(fileBuf));
  uint8_t* buf = static_cast<uint8_t*>(malloc(len + 1));
  if (!buf) return false;
  const int rc = mbedtls_base64_encode(buf, len + 1, &len, reinterpret_cast<const uint8_t*>(&fileBuf),
                                       sizeof(fileBuf));
  if (rc == 0) {
    buf[len] = '\0';
    out = reinterpret_cast<char*>(buf);
  }
  free(buf);
  return rc == 0;
}

bool decodeSlot(const String& in, ProfileSlot& out) {
  size_t len = 0;
  const int rc = mbedtls_base64_decode(reinterpret_cast<uint8_t*>(&fileBuf), sizeof(fileBuf), &len,
                                       reinterpret_cast<const uint8_t*>(in.c_str()), in.length());
  if (rc != 0 || len != sizeof(fileBuf) || fileBuf.magic != kSlotMagic ||
      fileBuf.version != kSlotVersion) {
    return false;
  }
  out = fileBuf.slot;
  out.name[sizeof(out.name) - 1] = '\0';
  clamp(out.settings);
  return true;
}

uint8_t loadActiveSlot() {
  const uint8_t s = prefs.getUChar("slot", 0);
  return s < cfg::kProfileSlots ? s : 0;
}

void saveActiveSlot(uint8_t slot) { prefs.putUChar("slot", slot); }

uint8_t loadMicGain() {
  return constrain(prefs.getUChar("micGain", cfg::kMicGainDefault), cfg::kMicGainMin, cfg::kMicGainMax);
}

void saveMicGain(uint8_t gain) { prefs.putUChar("micGain", gain); }

void loadWifi(WifiSettings& w) {
  w.ssid = prefs.getString("ssid", "");
  w.password = prefs.getString("pass", "");
}

void saveWifi(const WifiSettings& w) {
  prefs.putString("ssid", w.ssid);
  prefs.putString("pass", w.password);
}

void clamp(DetectorSettings& s) {
  s.similarityThreshold = constrain(s.similarityThreshold, 0.3f, 0.99f);
  s.periodicityThreshold = constrain(s.periodicityThreshold, 0.1f, 0.95f);
  s.levelThreshold = constrain(s.levelThreshold, 0.05f, 1.0f);
  s.onDelayMs = constrain(s.onDelayMs, (uint16_t)50, (uint16_t)3000);
  s.offDelayMs = constrain(s.offDelayMs, (uint16_t)100, (uint16_t)5000);
  s.minShotSeconds = constrain(s.minShotSeconds, 0.0f, 60.0f);
}

}  // namespace settings
