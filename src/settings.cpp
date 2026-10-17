/**
 * @file settings.cpp
 * @brief Persistenz: Profil-Slots im LittleFS, kleine Werte im NVS.
 */
#include "settings.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

#include <esp_heap_caps.h>
#include <stddef.h>

#include <algorithm>
#include <memory>
#include <new>

namespace {

constexpr const char* kNamespace = "coffeetimer";

/// Kennung und Version des Slot-Dateiformats. Bei Änderungen an PumpProfile,
/// NoiseProfile oder DetectorSettings die Version erhöhen.
constexpr uint32_t kSlotMagic = 0x43544D50;  // "CTMP"
constexpr uint32_t kSlotVersion = 8;

/// Binäres Dateiformat eines Slots.
struct SlotFile {
  uint32_t magic;
  uint32_t version;
  ProfileSlot slot;
};

// ---------------------------------------------------------------------------
// Altes Dateiformat v5 (bis Firmware 1.5): 24 Bänder, 6 Klangmuster, 2 je
// Störgeräusch. Wird beim Laden/Import auf die heutige Bandzahl umgerechnet.
// Felder exakt in derselben Reihenfolge wie damals (Binärformat!).
// ---------------------------------------------------------------------------
namespace v5 {
constexpr uint32_t kVersion = 5;
constexpr size_t kBands = 24;
constexpr size_t kTemplates = 6;
constexpr size_t kNoiseTemplates = 2;

struct PumpProfile {
  bool valid;
  uint8_t templateCount;
  uint8_t recordings;
  float bgBandDb[kBands];
  float templates[kTemplates][kBands];
  float bgRmsDb;
  float pumpRmsDb;
  bool periodic;
  float periodLag;
  float rhythm[cfg::kAcfLen];
  float rhythmThreshold;
  float steadyThreshold;
  float quality;
  float micGain;
};
struct NoiseProfile {
  struct Entry {
    uint8_t templateCount;
    float templates[kNoiseTemplates][kBands];
    float rhythm[cfg::kAcfLen];
  };
  uint8_t count;
  Entry entries[cfg::kMaxNoises];
};
struct ProfileSlot {
  char name[::ProfileSlot::kNameLen];
  PumpProfile pump;
  NoiseProfile noise;
  DetectorSettings settings;
};
struct SlotFile {
  uint32_t magic;
  uint32_t version;
  ProfileSlot slot;
};
// Größe der Dateien von Firmware 1.5 – schützt vor versehentlichen Änderungen oben.
static_assert(sizeof(SlotFile) == 8536, "v5-Format muss unverändert bleiben");

/// Bandwerte von 24 auf cfg::kNumBands Bänder umrechnen (linear über log. Frequenz).
void resample(const float* in, float* out) {
  for (size_t c = 0; c < cfg::kNumBands; ++c) {
    // Bandmitte c im neuen Raster -> Position im alten Raster (beide log. 80..7000 Hz)
    float pos = (c + 0.5f) * kBands / cfg::kNumBands - 0.5f;
    pos = constrain(pos, 0.0f, kBands - 1.0f);
    const size_t i = static_cast<size_t>(pos);
    const size_t j = i + 1 < kBands ? i + 1 : i;
    const float t = pos - i;
    out[c] = in[i] * (1.0f - t) + in[j] * t;
  }
}

/// Klangmuster umrechnen und wieder mittelwertfrei mit Länge 1 machen.
void resampleTemplate(const float* in, float* out) {
  resample(in, out);
  float mean = 0.0f;
  for (size_t b = 0; b < cfg::kNumBands; ++b) mean += out[b];
  mean /= cfg::kNumBands;
  float norm = 0.0f;
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    out[b] -= mean;
    norm += out[b] * out[b];
  }
  norm = sqrtf(norm);
  for (size_t b = 0; b < cfg::kNumBands; ++b) out[b] = norm > 1e-6f ? out[b] / norm : 0.0f;
}

void convertPump(const PumpProfile& in, ::PumpProfile& out) {
  out = ::PumpProfile{};
  out.valid = in.valid;
  out.templateCount = static_cast<uint8_t>(std::min<size_t>(in.templateCount, cfg::kMaxTemplates));
  out.recordings = in.recordings;
  resample(in.bgBandDb, out.bgBandDb);
  for (size_t t = 0; t < out.templateCount; ++t) resampleTemplate(in.templates[t], out.templates[t]);
  out.bgRmsDb = in.bgRmsDb;
  out.pumpRmsDb = in.pumpRmsDb;
  out.periodic = in.periodic;
  out.periodLag = in.periodLag;
  memcpy(out.rhythm, in.rhythm, sizeof(out.rhythm));
  out.rhythmThreshold = in.rhythmThreshold;
  out.steadyThreshold = in.steadyThreshold;
  out.quality = in.quality;
  out.micGain = in.micGain;
}

void convertNoise(const NoiseProfile& in, ::NoiseProfile& out) {
  resetInPlace(out);
  out.count = static_cast<uint8_t>(std::min<size_t>(in.count, cfg::kMaxNoises));
  for (size_t i = 0; i < out.count; ++i) {
    const NoiseProfile::Entry& e = in.entries[i];
    ::NoiseProfile::Entry& o = out.entries[i];
    o.templateCount = static_cast<uint8_t>(std::min<size_t>(e.templateCount, kNoiseTemplates));
    for (size_t t = 0; t < o.templateCount; ++t) resampleTemplate(e.templates[t], o.templates[t]);
    memcpy(o.rhythm, e.rhythm, sizeof(o.rhythm));
  }
}

void convert(const ProfileSlot& in, ::ProfileSlot& out) {
  resetInPlace(out);
  memcpy(out.name, in.name, sizeof(out.name));
  convertPump(in.pump, out.pump);
  convertNoise(in.noise, out.noise);
  out.settings = in.settings;
}

/// Wandelt einen v5-Datenblock (Datei oder Import) um. false = kein gültiges v5-Format.
bool decode(const uint8_t* data, size_t len, ::ProfileSlot& out) {
  if (len != sizeof(SlotFile)) return false;
  std::unique_ptr<SlotFile> f(new SlotFile);  // ~8,5 kB, nur kurz -> Heap
  memcpy(f.get(), data, sizeof(SlotFile));
  if (f->magic != kSlotMagic || f->version != kVersion) return false;
  convert(f->slot, out);
  return true;
}
}  // namespace v5

// ---------------------------------------------------------------------------
// Formate v6/v7 (40 Bänder): Störgeräusche/Mühlen noch ohne eigenen
// Hintergrund. v6 = v7 ohne das letzte Feld "grinders".
// ---------------------------------------------------------------------------
namespace v7 {
struct Entry {
  uint8_t templateCount;
  float templates[cfg::kNoiseTemplates][cfg::kNumBands];
  float rhythm[cfg::kAcfLen];
};
struct NoiseProfile {
  uint8_t count;
  Entry entries[cfg::kMaxNoises];
};
struct GrinderProfile {
  uint8_t count;
  Entry entries[cfg::kMaxGrinders];
  char names[cfg::kMaxGrinders][::GrinderProfile::kNameLen];
};
struct ProfileSlot {
  char name[::ProfileSlot::kNameLen];
  ::PumpProfile pump;
  NoiseProfile noise;
  DetectorSettings settings;
  GrinderProfile grinders;
};
constexpr size_t kFileV7 = 2 * sizeof(uint32_t) + sizeof(ProfileSlot);
constexpr size_t kFileV6 = 2 * sizeof(uint32_t) + offsetof(ProfileSlot, grinders);

void convertEntry(const Entry& in, ::NoiseProfile::Entry& out) {
  out.templateCount = std::min<uint8_t>(in.templateCount, cfg::kNoiseTemplates);
  memcpy(out.templates, in.templates, sizeof(out.templates));
  memcpy(out.rhythm, in.rhythm, sizeof(out.rhythm));
}

bool decode(const uint8_t* data, size_t len, ::ProfileSlot& out) {
  if (len != kFileV6 && len != kFileV7) return false;
  uint32_t magic, version;
  memcpy(&magic, data, sizeof(magic));
  memcpy(&version, data + sizeof(magic), sizeof(version));
  if (magic != kSlotMagic || version != (len == kFileV6 ? 6u : 7u)) return false;
  std::unique_ptr<ProfileSlot> old(new ProfileSlot());  // ~20 kB, nur kurz -> Heap (genullt)
  memcpy(old.get(), data + 2 * sizeof(uint32_t), len - 2 * sizeof(uint32_t));
  resetInPlace(out);
  memcpy(out.name, old->name, sizeof(out.name));
  out.pump = old->pump;
  out.settings = old->settings;
  out.noise.count = std::min<uint8_t>(old->noise.count, cfg::kMaxNoises);
  for (size_t i = 0; i < out.noise.count; ++i) convertEntry(old->noise.entries[i], out.noise.entries[i]);
  out.grinders.count = std::min<uint8_t>(old->grinders.count, cfg::kMaxGrinders);
  for (size_t i = 0; i < out.grinders.count; ++i) {
    convertEntry(old->grinders.entries[i], out.grinders.entries[i]);
    memcpy(out.grinders.names[i], old->grinders.names[i], sizeof(out.grinders.names[i]));
  }
  return true;
}
}  // namespace v7

/**
 * Ältere Störgeräusche/Mühlen wurden gegen den Hintergrund des Pumpenprofils
 * gelernt: diesen (und Verstärkung/Schwelle des Profils) übernehmen.
 */
void adoptProfileBackground(::ProfileSlot& s) {
  auto adopt = [&](::NoiseProfile::Entry& e) {
    memcpy(e.bgBandDb, s.pump.bgBandDb, sizeof(e.bgBandDb));
    e.bgRmsDb = s.pump.bgRmsDb;
    e.levelDb = s.pump.pumpRmsDb;
    e.simThreshold = s.settings.similarityThreshold;
    e.micGain = s.pump.micGain;
  };
  for (size_t i = 0; i < s.noise.count; ++i) adopt(s.noise.entries[i]);
  for (size_t i = 0; i < s.grinders.count; ++i) adopt(s.grinders.entries[i]);
}

// Alte NVS-Formate (Firmware 1.3/1.4) für die einmalige Übernahme
constexpr uint32_t kLegacyVersion = 4;
struct LegacyNoise {
  uint32_t version;
  v5::NoiseProfile noise;
};

Preferences prefs;
bool fsOk = false;

/// Datei-Puffer (~20 kB) im PSRAM statt im knappen internen RAM.
SlotFile* fileBuf = nullptr;

/// Liest ältere Formate (v5/v6/v7) bzw. das aktuelle. false = ungültig.
/// migrated = true, wenn umgerechnet wurde (sollte neu gespeichert werden).
bool decodeAny(const uint8_t* data, size_t len, ProfileSlot& out, bool& migrated) {
  migrated = true;
  if (v5::decode(data, len, out) || v7::decode(data, len, out)) {
    adoptProfileBackground(out);
    return true;
  }
  migrated = false;
  if (len != sizeof(SlotFile)) return false;
  const SlotFile* f = reinterpret_cast<const SlotFile*>(data);
  if (f->magic != kSlotMagic || f->version != kSlotVersion) return false;
  out = f->slot;
  return true;
}

/// Wert begrenzen; ungültige Zahl (NaN/unendlich) -> Ersatzwert.
void fix(float& v, float lo, float hi, float fallback) {
  v = isfinite(v) ? constrain(v, lo, hi) : fallback;
}

/// Störgeräusch / Mühle: Musteranzahl, Schwelle, Verstärkung, Pegel plausibel.
void sanitizeEntry(NoiseProfile::Entry& e) {
  e.templateCount = std::min<uint8_t>(e.templateCount, cfg::kNoiseTemplates);
  fix(e.simThreshold, 0.3f, 0.99f, 0.75f);
  fix(e.micGain, cfg::kMicGainMin, cfg::kMicGainMax, cfg::kMicGainDefault);
  fix(e.bgRmsDb, -120.0f, 0.0f, -90.0f);
  fix(e.levelDb, -120.0f, 0.0f, -90.0f);
}

/**
 * Namen terminieren, Zähler begrenzen, Schwellen plausibilisieren – schützt
 * vor beschädigten Dateien und Importen (Zähler werden als Array-Index genutzt).
 */
void sanitize(ProfileSlot& s) {
  s.name[sizeof(s.name) - 1] = '\0';
  PumpProfile& p = s.pump;
  p.templateCount = std::min<uint8_t>(p.templateCount, cfg::kMaxTemplates);
  fix(p.micGain, cfg::kMicGainMin, cfg::kMicGainMax, cfg::kMicGainDefault);
  fix(p.periodLag, 0.0f, cfg::kMaxLag, 0.0f);
  fix(p.rhythmThreshold, -1.0f, 1.0f, 0.5f);
  fix(p.steadyThreshold, 0.5f, 20.0f, 3.0f);
  fix(p.bgRmsDb, -120.0f, 0.0f, cfg::kDefaultBgDb);
  fix(p.pumpRmsDb, -120.0f, 0.0f, cfg::kDefaultPumpDb);
  s.noise.count = std::min<uint8_t>(s.noise.count, cfg::kMaxNoises);
  for (size_t i = 0; i < s.noise.count; ++i) sanitizeEntry(s.noise.entries[i]);
  s.grinders.count = std::min<uint8_t>(s.grinders.count, cfg::kMaxGrinders);
  for (size_t i = 0; i < s.grinders.count; ++i) sanitizeEntry(s.grinders.entries[i]);
  for (auto& n : s.grinders.names) n[GrinderProfile::kNameLen - 1] = '\0';
  settings::clamp(s.settings);
}

/**
 * Firmware 1.6 markierte ein Störgeräusch als Mühle (NVS-Schlüssel
 * "grinderN"). Dieses wird in die eigene Mühlen-Liste verschoben.
 * @return true, wenn etwas verschoben wurde
 */
bool adoptMarkedGrinder(uint8_t slot, ProfileSlot& s) {
  const String key = "grinder" + String(slot + 1);
  if (!prefs.isKey(key.c_str())) return false;
  const int8_t idx = prefs.getChar(key.c_str(), -1);
  prefs.remove(key.c_str());
  if (idx < 0 || idx >= s.noise.count || s.grinders.count >= cfg::kMaxGrinders) return false;
  const uint8_t g = s.grinders.count++;
  s.grinders.entries[g] = s.noise.entries[idx];
  snprintf(s.grinders.names[g], GrinderProfile::kNameLen, "Mühle %u", (unsigned)(g + 1));
  memmove(&s.noise.entries[idx], &s.noise.entries[idx + 1],
          sizeof(NoiseProfile::Entry) * (s.noise.count - idx - 1));
  --s.noise.count;
  Serial.printf("[settings] Slot %u: Störgeräusch %d ist jetzt %s\n", slot + 1, idx + 1, s.grinders.names[g]);
  return true;
}

String slotPath(uint8_t slot) {
  return "/slot" + String(slot + 1) + ".bin";
}

/**
 * Speichern läuft über eine temporäre Datei. Wurde es unterbrochen, bevor die
 * fertige .tmp-Datei an ihren Platz kam, wird das hier nachgeholt. Existiert
 * die eigentliche Datei noch, ist die .tmp evtl. unvollständig -> verwerfen.
 */
void recoverTempFiles() {
  for (uint8_t slot = 0; slot < cfg::kProfileSlots; ++slot) {
    const String path = slotPath(slot);
    const String tmp = path + ".tmp";
    if (!LittleFS.exists(tmp)) continue;
    if (LittleFS.exists(path)) {
      LittleFS.remove(tmp);
    } else if (LittleFS.rename(tmp, path)) {
      Serial.printf("[settings] Slot %u aus temporärer Datei wiederhergestellt\n", slot + 1);
    }
  }
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

  // PumpProfile v4 = Format v5 ohne das letzte Feld (micGain).
  const size_t legacyProfileLen = sizeof(uint32_t) + sizeof(v5::PumpProfile) - sizeof(float);
  if (prefs.getBytesLength("profile") == legacyProfileLen) {
    std::unique_ptr<uint8_t[]> rawPtr(new uint8_t[sizeof(uint32_t) + sizeof(v5::PumpProfile)]);
    uint8_t* raw = rawPtr.get();
    prefs.getBytes("profile", raw, legacyProfileLen);
    uint32_t version;
    memcpy(&version, raw, sizeof(version));
    if (version == kLegacyVersion) {
      std::unique_ptr<v5::PumpProfile> old(new v5::PumpProfile);
      memcpy(old.get(), raw + sizeof(uint32_t), sizeof(v5::PumpProfile) - sizeof(float));
      old->micGain = cfg::kMicGainDefault;
      v5::convertPump(*old, slot.pump);
      found = slot.pump.valid;
    }
  }
  if (found && prefs.getBytesLength("noise") == sizeof(LegacyNoise)) {
    std::unique_ptr<LegacyNoise> lnPtr(new LegacyNoise);
    LegacyNoise& ln = *lnPtr;
    prefs.getBytes("noise", &ln, sizeof(ln));
    if (ln.version == kLegacyVersion) v5::convertNoise(ln.noise, slot.noise);
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
  void* mem = heap_caps_malloc(sizeof(SlotFile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(SlotFile));
  fileBuf = new (mem) SlotFile();
  // Partition "spiffs" aus default_16MB.csv; beim ersten Start formatieren.
  fsOk = LittleFS.begin(true, "/littlefs", 5, "spiffs");
  if (!fsOk) Serial.println("[settings] FEHLER: Dateisystem nicht verfügbar");
  if (fsOk) recoverTempFiles();
  migrateLegacy();
}

void defaultSlot(uint8_t slot, ProfileSlot& out) {
  resetInPlace(out);
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
  const size_t n = f.read(reinterpret_cast<uint8_t*>(fileBuf), sizeof(SlotFile));
  f.close();
  bool migrated;
  if (!decodeAny(reinterpret_cast<const uint8_t*>(fileBuf), n, out, migrated)) {
    Serial.printf("[settings] Slot %u ungültig (Größe/Version)\n", slot + 1);
    return false;
  }
  if (adoptMarkedGrinder(slot, out)) migrated = true;
  sanitize(out);
  // Älteres Format: gleich im aktuellen Format speichern.
  if (migrated) {
    Serial.printf("[settings] Slot %u ins aktuelle Format übernommen\n", slot + 1);
    saveSlot(slot, out);
  }
  return true;
}

bool saveSlot(uint8_t slot, const ProfileSlot& in) {
  if (!fsOk || slot >= cfg::kProfileSlots) return false;
  fileBuf->magic = kSlotMagic;
  fileBuf->version = kSlotVersion;
  fileBuf->slot = in;

  // Erst vollständig in eine temporäre Datei schreiben, dann über die alte
  // umbenennen (LittleFS ersetzt dabei atomar): Bei Stromausfall bleibt immer
  // eine vollständige Datei übrig (siehe recoverTempFiles).
  const String path = slotPath(slot);
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t n = f.write(reinterpret_cast<const uint8_t*>(fileBuf), sizeof(SlotFile));
  f.close();
  if (n != sizeof(SlotFile)) {
    LittleFS.remove(tmp);
    Serial.printf("[settings] FEHLER beim Schreiben von Slot %u\n", slot + 1);
    return false;
  }
  if (LittleFS.rename(tmp, path)) return true;
  // Falls das Dateisystem das Überschreiben ablehnt: alte Datei weg, dann
  // umbenennen (die fertige .tmp wird notfalls beim Start übernommen).
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

bool deleteSlot(uint8_t slot) {
  if (!slotExists(slot)) return true;
  return LittleFS.remove(slotPath(slot));
}

bool encodeSlot(const ProfileSlot& in, String& out) {
  fileBuf->magic = kSlotMagic;
  fileBuf->version = kSlotVersion;
  fileBuf->slot = in;
  size_t len = 0;
  mbedtls_base64_encode(nullptr, 0, &len, reinterpret_cast<const uint8_t*>(fileBuf), sizeof(SlotFile));
  uint8_t* buf = static_cast<uint8_t*>(malloc(len + 1));
  if (!buf) return false;
  const int rc = mbedtls_base64_encode(buf, len + 1, &len, reinterpret_cast<const uint8_t*>(fileBuf),
                                       sizeof(SlotFile));
  if (rc == 0) {
    buf[len] = '\0';
    out = reinterpret_cast<char*>(buf);
  }
  free(buf);
  return rc == 0;
}

bool decodeSlot(const String& in, ProfileSlot& out) {
  size_t len = 0;
  const int rc = mbedtls_base64_decode(reinterpret_cast<uint8_t*>(fileBuf), sizeof(SlotFile), &len,
                                       reinterpret_cast<const uint8_t*>(in.c_str()), in.length());
  bool migrated;  // Exporte älterer Firmware werden umgerechnet
  if (rc != 0 || !decodeAny(reinterpret_cast<const uint8_t*>(fileBuf), len, out, migrated)) return false;
  sanitize(out);
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
