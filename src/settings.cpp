// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file settings.cpp
 * @brief Persistenz: Profil-Slots im LittleFS, kleine Werte im NVS.
 */
#include "settings.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

constexpr const char* kNamespace = "coffeetimer";

// ---------------------------------------------------------------------------
// Dateien eines Slots: je Teil eine eigene Datei (Pumpe / Störgeräusche /
// Mühlen). Gespeichert wird nur der geänderte Teil – die anderen Dateien
// werden dabei nicht angefasst.
// ---------------------------------------------------------------------------
constexpr uint32_t kPartVersion = 2;
constexpr uint32_t kPumpMagic = 0x50544350;   // "PCTP"
constexpr uint32_t kNoiseMagic = 0x4E544350;  // "PCTN"
constexpr uint32_t kGrindMagic = 0x47544350;  // "PCTG"
constexpr uint32_t kTrainMagic = 0x54544350;  // "PCTT" Trainingsdaten des Klassifikators
/// Trainingsdaten haben ihr eigenes, unverändertes Format.
constexpr uint32_t kTrainVersion = 1;
/// Größte zulässige Teil-Datei – schützt vor defekten Dateien.
constexpr size_t kMaxPartBytes = 1024;

/// Kopf jeder Teil-Datei: Kennung, Version und Nutzdatengröße (prüft Format).
struct PartHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t size;
};

// Dateiformat = die Strukturen 1:1. Die Größen sichern es gegen versehentliche
// Änderungen ab – wer eine Struktur ändert, muss kPartVersion erhöhen.
static_assert(sizeof(PumpProfile) == 24, "Profil-Format geändert: kPartVersion erhöhen");
static_assert(sizeof(DetectorSettings) == 12, "Profil-Format geändert: kPartVersion erhöhen");
static_assert(sizeof(NoiseProfile) == 40, "Profil-Format geändert: kPartVersion erhöhen");
static_assert(sizeof(GrinderProfile) == 88, "Profil-Format geändert: kPartVersion erhöhen");

Preferences prefs;
bool fsOk = false;

/// Wert begrenzen; ungültige Zahl (NaN/unendlich) -> Ersatzwert.
void fix(float& v, float lo, float hi, float fallback) {
  v = isfinite(v) ? constrain(v, lo, hi) : fallback;
}

/// Störgeräusch / Mühle: Verstärkung und Pegel plausibel.
void sanitizeEntry(NoiseProfile::Entry& e) {
  fix(e.micGain, cfg::kMicGainMin, cfg::kMicGainMax, cfg::kMicGainDefault);
  fix(e.bgRmsDb, -120.0f, 0.0f, -90.0f);
  fix(e.levelDb, -120.0f, 0.0f, -90.0f);
}

/**
 * Namen terminieren, Zähler begrenzen, Werte plausibilisieren – schützt vor
 * beschädigten Dateien und Importen (Zähler werden als Array-Index genutzt).
 */
void sanitize(ProfileSlot& s) {
  s.name[sizeof(s.name) - 1] = '\0';
  PumpProfile& p = s.pump;
  fix(p.micGain, cfg::kMicGainMin, cfg::kMicGainMax, cfg::kMicGainDefault);
  fix(p.steadyThreshold, 0.5f, 20.0f, 3.0f);
  fix(p.bgRmsDb, -120.0f, 0.0f, cfg::kDefaultBgDb);
  fix(p.pumpRmsDb, -120.0f, 0.0f, cfg::kDefaultPumpDb);
  fix(p.quality, 0.0f, 100.0f, 0.0f);
  s.noise.count = std::min<uint8_t>(s.noise.count, cfg::kMaxNoises);
  for (size_t i = 0; i < s.noise.count; ++i) sanitizeEntry(s.noise.entries[i]);
  s.grinders.count = std::min<uint8_t>(s.grinders.count, cfg::kMaxGrinders);
  for (size_t i = 0; i < s.grinders.count; ++i) sanitizeEntry(s.grinders.entries[i]);
  for (auto& n : s.grinders.names) n[GrinderProfile::kNameLen - 1] = '\0';
  settings::clamp(s.settings);
}

/// Ein Datenblock einer Teil-Datei (Zeiger + Größe).
struct Chunk {
  void* data;
  size_t size;
};

/**
 * Aufbau eines Teils – EINE Stelle für Speichern, Laden, Export und Import.
 * @return Anzahl der Blöcke in chunks (0 = unbekannter Teil)
 */
size_t partLayout(ProfileSlot& s, uint8_t part, Chunk (&chunks)[3], uint32_t& magic) {
  switch (part) {
    case settings::kPartPump:
      magic = kPumpMagic;
      chunks[0] = {s.name, sizeof(s.name)};
      chunks[1] = {&s.pump, sizeof(s.pump)};
      chunks[2] = {&s.settings, sizeof(s.settings)};
      return 3;
    case settings::kPartNoise:
      magic = kNoiseMagic;
      chunks[0] = {&s.noise, sizeof(s.noise)};
      return 1;
    case settings::kPartGrinders:
      magic = kGrindMagic;
      chunks[0] = {&s.grinders, sizeof(s.grinders)};
      return 1;
  }
  return 0;
}

/// Teil -> Bytes (Kopf + Daten, aktuelles Format). Gemeinsam für Datei und Export.
bool serializePart(const ProfileSlot& in, uint8_t part, std::vector<uint8_t>& out) {
  Chunk c[3];
  uint32_t magic;
  const size_t n = partLayout(const_cast<ProfileSlot&>(in), part, c, magic);  // nur gelesen
  if (!n) return false;
  PartHeader h{magic, kPartVersion, 0};
  for (size_t i = 0; i < n; ++i) h.size += c[i].size;
  out.resize(sizeof(h) + h.size);
  memcpy(out.data(), &h, sizeof(h));
  size_t pos = sizeof(h);
  for (size_t i = 0; i < n; ++i) {
    memcpy(out.data() + pos, c[i].data, c[i].size);
    pos += c[i].size;
  }
  return true;
}

/// Bytes (Kopf + Daten) -> Teil. Schreibt nur den angegebenen Teil von out.
bool parsePart(const uint8_t* data, size_t len, uint8_t part, ProfileSlot& out) {
  Chunk c[3];
  uint32_t magic;
  const size_t n = partLayout(out, part, c, magic);
  PartHeader h{};
  if (!n || len < sizeof(h)) return false;
  memcpy(&h, data, sizeof(h));
  if (h.magic != magic || h.version != kPartVersion || h.size != len - sizeof(h)) return false;
  data += sizeof(h);
  size_t expected = 0;
  for (size_t i = 0; i < n; ++i) expected += c[i].size;
  if (h.size != expected) return false;
  for (size_t i = 0; i < n; ++i) {
    memcpy(c[i].data, data, c[i].size);
    data += c[i].size;
  }
  return true;
}

String slotBase(uint8_t slot) { return "/slot" + String(slot + 1); }
String trainPath(uint8_t slot) { return slotBase(slot) + "_train.bin"; }

String partPath(uint8_t slot, uint8_t part) {
  switch (part) {
    case settings::kPartPump:     return slotBase(slot) + "_pump.bin";
    case settings::kPartNoise:    return slotBase(slot) + "_noise.bin";
    case settings::kPartGrinders: return slotBase(slot) + "_grinder.bin";
  }
  return "";
}

/**
 * Schreibt eine Datei: erst vollständig als .tmp, dann über die alte
 * umbenennen (LittleFS ersetzt atomar). Bei Stromausfall bleibt immer eine
 * vollständige Datei übrig (siehe recoverTempFiles).
 */
bool writeFile(const String& path, const std::vector<uint8_t>& data) {
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t written = f.write(data.data(), data.size());
  f.close();
  if (written != data.size()) {
    LittleFS.remove(tmp);
    Serial.printf("[settings] FEHLER beim Schreiben von %s\n", path.c_str());
    return false;
  }
  if (LittleFS.rename(tmp, path)) return true;
  // Falls das Dateisystem das Überschreiben ablehnt: alte Datei weg, dann umbenennen
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

/// Liest eine ganze Datei (höchstens maxBytes). false = fehlt oder zu groß.
bool readFile(const String& path, std::vector<uint8_t>& out, size_t maxBytes) {
  out.clear();
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t size = f.size();
  bool ok = size <= maxBytes;
  if (ok) {
    out.resize(size);  // > 4 kB -> PSRAM
    ok = f.read(out.data(), size) == size;
  }
  f.close();
  return ok;
}

/// Liest eine Teil-Datei. false = fehlt oder falsches Format (Teil dann unbrauchbar).
bool readPart(uint8_t slot, uint8_t part, ProfileSlot& out) {
  std::vector<uint8_t> buf;
  if (!readFile(partPath(slot, part), buf, kMaxPartBytes)) return false;
  const bool ok = parsePart(buf.data(), buf.size(), part, out);
  if (!ok) Serial.printf("[settings] %s ungültig (Größe/Version)\n", partPath(slot, part).c_str());
  return ok;
}

/**
 * Speichern läuft über temporäre Dateien. Wurde es unterbrochen, bevor die
 * fertige .tmp-Datei an ihren Platz kam, wird das hier nachgeholt. Existiert
 * die eigentliche Datei noch, ist die .tmp evtl. unvollständig -> verwerfen.
 */
void recoverTempFiles() {
  for (uint8_t slot = 0; slot < cfg::kProfileSlots; ++slot) {
    for (const String& path : {partPath(slot, settings::kPartPump), partPath(slot, settings::kPartNoise),
                               partPath(slot, settings::kPartGrinders), trainPath(slot)}) {
      const String tmp = path + ".tmp";
      if (!LittleFS.exists(tmp)) continue;
      if (LittleFS.exists(path)) {
        LittleFS.remove(tmp);
      } else if (LittleFS.rename(tmp, path)) {
        Serial.printf("[settings] %s aus temporärer Datei wiederhergestellt\n", path.c_str());
      }
    }
  }
}

}  // namespace

namespace settings {

void begin() {
  prefs.begin(kNamespace, false);
  prefs.remove("useCls");  // KI-Erkennung ist seit Firmware 2.0 immer an
  // Partition "spiffs" aus default_16MB.csv; beim ersten Start formatieren.
  fsOk = LittleFS.begin(true, "/littlefs", 5, "spiffs");
  if (!fsOk) Serial.println("[settings] FEHLER: Dateisystem nicht verfügbar");
  if (fsOk) recoverTempFiles();
}

void defaultSlot(uint8_t slot, ProfileSlot& out) {
  resetInPlace(out);
  snprintf(out.name, sizeof(out.name), "Profil %u", (unsigned)(slot + 1));

  // Standardprofil: erkannt wird jedes gleichmäßige Geräusch deutlich über
  // dem (automatisch nachgeführten) Hintergrund.
  PumpProfile& p = out.pump;
  p.valid = true;
  p.recordings = 0;
  p.bgRmsDb = cfg::kDefaultBgDb;
  p.pumpRmsDb = cfg::kDefaultPumpDb;
  p.steadyThreshold = cfg::kDefaultSteadyDb;
  p.micGain = cfg::kMicGainDefault;
}

bool slotExists(uint8_t slot) {
  return fsOk && slot < cfg::kProfileSlots && LittleFS.exists(partPath(slot, kPartPump));
}

bool loadSlot(uint8_t slot, ProfileSlot& out) {
  defaultSlot(slot, out);
  if (!slotExists(slot)) return false;
  // Fehlt bzw. defekt eine Mühlen- oder Störgeräusch-Datei, ist dieser Teil
  // leer – der Rest bleibt nutzbar.
  if (!readPart(slot, kPartPump, out)) {
    defaultSlot(slot, out);
    return false;
  }
  if (!readPart(slot, kPartNoise, out)) resetInPlace(out.noise);
  if (!readPart(slot, kPartGrinders, out)) resetInPlace(out.grinders);
  sanitize(out);
  return true;
}

bool saveSlot(uint8_t slot, const ProfileSlot& in, uint8_t parts) {
  if (!fsOk || slot >= cfg::kProfileSlots) return false;
  // Die Pumpen-Datei kennzeichnet einen belegten Slot: beim ersten Speichern
  // (z. B. zuerst eine Mühle eingelernt) immer mitschreiben.
  if (!LittleFS.exists(partPath(slot, kPartPump))) parts |= kPartPump;
  bool ok = true;
  for (uint8_t part : {kPartPump, kPartNoise, kPartGrinders}) {
    if (!(parts & part)) continue;
    std::vector<uint8_t> buf;
    ok = serializePart(in, part, buf) && writeFile(partPath(slot, part), buf) && ok;
  }
  return ok;
}

bool deleteSlot(uint8_t slot) {
  if (!fsOk || slot >= cfg::kProfileSlots) return false;
  bool ok = true;
  for (const String& p : {partPath(slot, kPartPump), partPath(slot, kPartNoise), partPath(slot, kPartGrinders),
                          trainPath(slot)}) {
    if (LittleFS.exists(p)) ok = LittleFS.remove(p) && ok;
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Trainingsdaten: Kopf (Kennung, Version, Anzahl Aufnahmen), dann je Aufnahme
// Art, Index, Anzahl Klangbilder und die Klangbilder (int8 × kNumBands).
// ---------------------------------------------------------------------------
struct TrainRecordHeader {
  uint8_t kind;
  uint8_t index;
  uint16_t count;
};

/// Trainingsdaten -> Bytes (Datei-Format). Gemeinsam für Datei und Export.
void serializeTraining(const classifier::TrainingSet& set, std::vector<uint8_t>& out) {
  const PartHeader h{kTrainMagic, kTrainVersion, static_cast<uint32_t>(set.records.size())};
  size_t total = sizeof(h);
  for (const classifier::Record& r : set.records) total += sizeof(TrainRecordHeader) + r.frames.size();
  out.resize(total);  // > 4 kB -> PSRAM
  uint8_t* p = out.data();
  memcpy(p, &h, sizeof(h));
  p += sizeof(h);
  for (const classifier::Record& r : set.records) {
    const TrainRecordHeader rh{static_cast<uint8_t>(r.kind), r.index, static_cast<uint16_t>(r.count())};
    memcpy(p, &rh, sizeof(rh));
    p += sizeof(rh);
    memcpy(p, r.frames.data(), r.frames.size());
    p += r.frames.size();
  }
}

/// Bytes -> Trainingsdaten. Prüft Kennung, Version und jede Längenangabe.
bool parseTraining(const uint8_t* data, size_t len, classifier::TrainingSet& set) {
  set.records.clear();
  PartHeader h{};
  if (len < sizeof(h)) return false;
  memcpy(&h, data, sizeof(h));
  if (h.magic != kTrainMagic || h.version != kTrainVersion || h.size > 128) return false;
  size_t pos = sizeof(h);
  for (uint32_t i = 0; i < h.size; ++i) {
    TrainRecordHeader rh{};
    if (pos + sizeof(rh) > len) return false;
    memcpy(&rh, data + pos, sizeof(rh));
    pos += sizeof(rh);
    const size_t bytes = static_cast<size_t>(rh.count) * cfg::kNumBands;
    if (rh.kind > 3 || rh.count > classifier::kMaxFramesPerRecord || pos + bytes > len) return false;
    classifier::Record r;
    r.kind = static_cast<classifier::Kind>(rh.kind);
    r.index = rh.index;
    r.frames.assign(data + pos, data + pos + bytes);
    pos += bytes;
    set.records.push_back(std::move(r));
  }
  return pos == len;
}

bool loadTraining(uint8_t slot, classifier::TrainingSet& set) {
  set.records.clear();
  std::vector<uint8_t> buf;
  if (!fsOk || !readFile(trainPath(slot), buf, 1024 * 1024)) return false;
  if (!parseTraining(buf.data(), buf.size(), set)) {
    set.records.clear();
    Serial.printf("[settings] %s ungültig – Trainingsdaten verworfen\n", trainPath(slot).c_str());
    return false;
  }
  return true;
}

bool saveTraining(uint8_t slot, const classifier::TrainingSet& set) {
  if (!fsOk || slot >= cfg::kProfileSlots) return false;
  std::vector<uint8_t> buf;
  serializeTraining(set, buf);
  return writeFile(trainPath(slot), buf);
}

namespace {
/// Bytes -> Base64-Text.
bool toBase64(const std::vector<uint8_t>& in, String& out) {
  size_t len = 0;
  mbedtls_base64_encode(nullptr, 0, &len, in.data(), in.size());
  std::vector<uint8_t> text(len + 1);
  if (mbedtls_base64_encode(text.data(), text.size(), &len, in.data(), in.size()) != 0) return false;
  text[len] = '\0';
  out = reinterpret_cast<const char*>(text.data());
  return true;
}

/// Base64-Text -> Bytes.
bool fromBase64(const String& in, std::vector<uint8_t>& out) {
  size_t len = 0;
  const auto* src = reinterpret_cast<const uint8_t*>(in.c_str());
  mbedtls_base64_decode(nullptr, 0, &len, src, in.length());
  out.resize(len);
  if (mbedtls_base64_decode(out.data(), out.size(), &len, src, in.length()) != 0) return false;
  out.resize(len);
  return true;
}
}  // namespace

bool encodeTraining(const classifier::TrainingSet& set, String& out) {
  std::vector<uint8_t> buf;
  serializeTraining(set, buf);
  return toBase64(buf, out);
}

bool decodeTraining(const String& in, classifier::TrainingSet& set) {
  std::vector<uint8_t> buf;
  return fromBase64(in, buf) && parseTraining(buf.data(), buf.size(), set);
}

bool encodePart(const ProfileSlot& in, uint8_t part, String& out) {
  std::vector<uint8_t> buf;
  return serializePart(in, part, buf) && toBase64(buf, out);
}

bool decodePart(const String& in, uint8_t part, ProfileSlot& out) {
  std::vector<uint8_t> buf;
  if (!fromBase64(in, buf) || buf.size() > kMaxPartBytes || !parsePart(buf.data(), buf.size(), part, out)) {
    return false;
  }
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
  s.levelThreshold = constrain(s.levelThreshold, 0.05f, 1.0f);
  s.onDelayMs = constrain(s.onDelayMs, (uint16_t)50, (uint16_t)3000);
  s.offDelayMs = constrain(s.offDelayMs, (uint16_t)100, (uint16_t)5000);
  s.minShotSeconds = constrain(s.minShotSeconds, 0.0f, 60.0f);
}

}  // namespace settings
