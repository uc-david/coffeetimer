// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file settings.h
 * @brief Persistente Daten: Profil-Slots, Mikrofon- und WLAN-Einstellungen.
 *
 * - Profile (Pumpe, Störgeräusche, Schwellen) liegen als Dateien im
 *   LittleFS-Dateisystem (Partition "spiffs", 3,4 MB): je Slot eine Datei.
 *   Der kleine NVS-Speicher (20 kB) reichte für die großen Profile nicht
 *   zuverlässig – Schreibfehler blieben dort unbemerkt.
 * - Kleine Werte (aktiver Slot, Mikrofon-Verstärkung, WLAN) liegen im NVS
 *   (Arduino-Klasse Preferences).
 * Beides überlebt Neustarts und Firmware-Updates (auch OTA).
 */
#pragma once

#include <Arduino.h>

#include <new>

#include "classifier.h"
#include "config.h"

/**
 * Vom Teach-Modus gelernter akustischer Fingerabdruck der Pumpe.
 *
 * Statt eines einzigen gemittelten Spektrums enthält das Profil mehrere
 * Klangmuster, weil sich das Geräusch während eines Bezugs ändert
 * (Leerlauf -> Druckaufbau -> voller Druck) und ein Leerbezug anders klingt
 * als ein echter Bezug. Erkannt wird, wenn eines der Muster passt.
 */
struct PumpProfile {
  bool valid = false;                   ///< true, sobald ein Teach gespeichert wurde
  uint8_t templateCount = 0;            ///< Anzahl gültiger Klangmuster
  uint8_t recordings = 0;               ///< Anzahl eingeflossener Teach-Aufnahmen
  float bgBandDb[cfg::kNumBands] = {};  ///< mittleres Hintergrundspektrum
  /// Klangmuster: Anteil über dem Hintergrund je Band, mittelwertfrei und auf
  /// Länge 1 normiert (beschreibt die Klangfarbe, nicht die Lautstärke).
  float templates[cfg::kMaxTemplates][cfg::kNumBands] = {};
  float bgRmsDb = -90.0f;               ///< mittlerer Hintergrundpegel (dBFS)
  /// Referenzpegel der Pumpe (dBFS) – bewusst der *leise* Teil der Aufnahme
  /// (10 %-Perzentil), damit auch gedämpfter Lauf unter Druck erfasst wird.
  float pumpRmsDb = -90.0f;
  bool periodic = false;                ///< Pumpe hat einen ausgeprägten Takt
  float periodLag = 0.0f;               ///< Taktlänge in Samples (z. B. 320 = 50 Hz)
  /// Rhythmus-Fingerabdruck: mittlere Rhythmus-Kurve der Pumpe
  /// (mittelwertfrei, Länge 1; siehe FeatureExtractor::acfCurve()).
  float rhythm[cfg::kAcfLen] = {};
  /// Mindest-Ähnlichkeit der Rhythmus-Form (aus dem Teach berechnet).
  float rhythmThreshold = 0.5f;
  /// Maximale Pegelschwankung (dB, Standardabweichung über ~0,5 s), aus dem
  /// Teach berechnet. Kurze, unruhige Geräusche (Niesen, Sprache) liegen darüber.
  float steadyThreshold = 3.0f;
  float quality = 0.0f;                 ///< Qualitätswert der (letzten) Aufnahme 0..100
  /// Mikrofon-Verstärkung beim Teach. Wird sie später geändert, gleicht die
  /// Erkennung die Pegel rechnerisch aus (muss das letzte Feld bleiben, siehe
  /// Migration in settings.cpp).
  float micGain = cfg::kMicGainDefault;

  /// true = Standardprofil ohne Teach (nur Pegel + Gleichmäßigkeit).
  bool isDefault() const { return valid && recordings == 0; }

  /// Taktfrequenz in Hz (0 = kein Takt).
  float periodHz() const { return periodic && periodLag > 0 ? cfg::kSampleRate / periodLag : 0.0f; }
};

/**
 * Gelernte Störgeräusche (z. B. Milchaufschäumer). Klingt ein Geräusch einem
 * Störgeräusch ähnlicher als der Pumpe, wird es ignoriert.
 *
 * Jeder Eintrag ist eigenständig: Er bringt seinen eigenen Hintergrund, Pegel,
 * Schwelle und die Mikrofon-Verstärkung seiner Aufnahme mit. Ein späterer
 * Pumpen-Teach (anderer Hintergrund, andere Verstärkung) ändert daran nichts.
 */
struct NoiseProfile {
  struct Entry {
    uint8_t templateCount = 0;
    float templates[cfg::kNoiseTemplates][cfg::kNumBands] = {};  ///< wie PumpProfile
    float rhythm[cfg::kAcfLen] = {};                             ///< wie PumpProfile
    float bgBandDb[cfg::kNumBands] = {};  ///< Hintergrundspektrum bei der Aufnahme
    float bgRmsDb = -90.0f;               ///< Hintergrundpegel bei der Aufnahme
    float levelDb = -90.0f;               ///< typischer (leiser) Pegel des Geräuschs
    float simThreshold = 0.75f;           ///< eigene Mindest-Ähnlichkeit (aus der Aufnahme)
    float micGain = cfg::kMicGainDefault; ///< Mikrofon-Verstärkung bei der Aufnahme
  };
  uint8_t count = 0;
  Entry entries[cfg::kMaxNoises];
};

/**
 * Gelernte Mühlen – getrennt von den Störgeräuschen. Werden wie diese bei der
 * Pumpenerkennung ignoriert und lösen zusätzlich die Mühlen-Tara aus.
 * Jede Mühle kann mehrere Aufnahmen enthalten (leer / mit Bohnen, Mahlgrad).
 */
struct GrinderProfile {
  static constexpr size_t kNameLen = 16;
  uint8_t count = 0;
  NoiseProfile::Entry entries[cfg::kMaxGrinders];
  char names[cfg::kMaxGrinders][kNameLen] = {};  ///< frei wählbar (UTF-8)
};

/// Vom Benutzer einstellbare Erkennungsparameter. Klangmuster- und
/// Takt-Schwelle werden beim Speichern eines Teach-Ergebnisses automatisch
/// passend zur Aufnahme gesetzt und können danach feinjustiert werden.
struct DetectorSettings {
  /// Mindest-Ähnlichkeit (Kosinus) zum besten Klangmuster (0..1).
  float similarityThreshold = 0.75f;
  /// Mindest-Takt-Stärke (Autokorrelation beim Pumpentakt, 0..1).
  float periodicityThreshold = 0.40f;
  /// Mindestpegel als Anteil des Abstands Hintergrund -> Pumpe (0..1).
  /// 0.5 = Pegel muss mindestens in der Mitte zwischen beiden liegen.
  float levelThreshold = 0.5f;
  /// So lange muss das Geräusch anliegen, bevor "Pumpe EIN" gemeldet wird.
  uint16_t onDelayMs = 300;
  /// So lange muss das Geräusch fehlen, bevor "Pumpe AUS" gemeldet wird.
  uint16_t offDelayMs = 700;
  /// Kürzere Pumpvorgänge (z. B. Spülen) werden nicht in die Historie übernommen.
  float minShotSeconds = 5.0f;
};

/// Inhalt eines Profil-Speicherplatzes.
struct ProfileSlot {
  static constexpr size_t kNameLen = 24;
  char name[kNameLen] = {};     ///< frei wählbarer Name (UTF-8)
  PumpProfile pump;
  NoiseProfile noise;
  DetectorSettings settings;
  GrinderProfile grinders;      ///< muss das letzte Feld bleiben (Migration v6 in settings.cpp)
};

/// WLAN-Zugangsdaten.
struct WifiSettings {
  String ssid;
  String password;
};

/// Setzt ein (großes) Objekt auf seine Standardwerte zurück, ohne ein
/// Temporärobjekt auf dem Stack anzulegen (ProfileSlot ist ~20 kB groß).
template <typename T>
void resetInPlace(T& v) {
  v.~T();
  new (&v) T();
}

/// Laden / Speichern aller persistenten Daten.
namespace settings {

/// Öffnet NVS und Dateisystem; übernimmt ein altes NVS-Profil in Slot 1.
void begin();

/// Standardprofil (ohne Teach) für einen leeren Slot.
void defaultSlot(uint8_t slot, ProfileSlot& out);

/// Lädt einen Slot. false = leer/ungültig (out enthält dann das Standardprofil).
bool loadSlot(uint8_t slot, ProfileSlot& out);

/// Teile eines Slots – jeder liegt in einer eigenen Datei.
enum Part : uint8_t {
  kPartPump = 1,      ///< /slotN_pump.bin: Name, Pumpe, Einstellungen
  kPartNoise = 2,     ///< /slotN_noise.bin: Störgeräusche
  kPartGrinders = 4,  ///< /slotN_grinder.bin: Mühlen
  kPartAll = 7,
};

/// Speichert die angegebenen Teile eines Slots (nur deren Dateien werden
/// geschrieben). false = Schreibfehler (wird geprüft, nicht nur versucht).
bool saveSlot(uint8_t slot, const ProfileSlot& in, uint8_t parts = kPartAll);

/// Löscht einen Slot (danach wieder Standardprofil).
bool deleteSlot(uint8_t slot);

/// Ist der Slot belegt (gespeicherte Datei vorhanden)?
bool slotExists(uint8_t slot);

/// Slot als Text (Base64) für Export / Import über die Webseite.
bool encodeSlot(const ProfileSlot& in, String& out);
bool decodeSlot(const String& in, ProfileSlot& out);

/// Trainingsdaten des Klassifikators (eigene Datei /slotN_train.bin).
/// load: false = keine/ungültige Datei (set ist dann leer).
bool loadTraining(uint8_t slot, classifier::TrainingSet& set);
bool saveTraining(uint8_t slot, const classifier::TrainingSet& set);
/// Trainingsdaten als Text (Base64, gleicher Aufbau wie die Datei) bzw. zurück.
bool encodeTraining(const classifier::TrainingSet& set, String& out);
bool decodeTraining(const String& in, classifier::TrainingSet& set);

/// Ein Teil eines Slots (kPartPump / kPartNoise / kPartGrinders) als Text
/// (Base64, gleicher Aufbau wie die Datei) bzw. zurück. decodePart schreibt
/// nur den angegebenen Teil von out.
bool encodePart(const ProfileSlot& in, uint8_t part, String& out);
bool decodePart(const String& in, uint8_t part, ProfileSlot& out);

uint8_t loadActiveSlot();
void saveActiveSlot(uint8_t slot);

/// KI-Erkennung (Klassifikator) an/aus – gilt für alle Profile.
bool loadUseClassifier();
void saveUseClassifier(bool on);

uint8_t loadMicGain();
void saveMicGain(uint8_t gain);

void loadWifi(WifiSettings& w);
void saveWifi(const WifiSettings& w);

/// Plausibilisiert Einstellungen (z. B. aus der Webseite) auf gültige Bereiche.
void clamp(DetectorSettings& s);

}  // namespace settings
