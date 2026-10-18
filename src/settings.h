// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file settings.h
 * @brief Persistente Daten: Profil-Slots, Mikrofon- und WLAN-Einstellungen.
 *
 * - Profile liegen als Dateien im LittleFS-Dateisystem (Partition "spiffs",
 *   3,4 MB): je Slot und Teil eine Datei (Pumpe, Störgeräusche, Mühlen,
 *   KI-Trainingsdaten).
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
 * Pegel der Pumpe aus dem Teach. WAS zu hören ist, entscheidet die KI-Erkennung
 * (classifier.h, Trainingsdaten in eigener Datei); das Profil liefert, WIE LAUT
 * und WIE GLEICHMÄSSIG die Pumpe ist – damit leise oder unruhige Geräusche gar
 * nicht erst in Frage kommen.
 */
struct PumpProfile {
  bool valid = false;                   ///< true, sobald ein Teach gespeichert wurde
  uint8_t recordings = 0;               ///< Anzahl eingeflossener Teach-Aufnahmen
  float bgRmsDb = -90.0f;               ///< mittlerer Hintergrundpegel (dBFS)
  /// Referenzpegel der Pumpe (dBFS) – bewusst der *leise* Teil der Aufnahme
  /// (10 %-Perzentil), damit auch gedämpfter Lauf unter Druck erfasst wird.
  float pumpRmsDb = -90.0f;
  /// Maximale Pegelschwankung (dB, Standardabweichung über ~0,5 s), aus dem
  /// Teach berechnet. Kurze, unruhige Geräusche (Niesen, Sprache) liegen darüber.
  float steadyThreshold = 3.0f;
  float quality = 0.0f;                 ///< Qualitätswert der (letzten) Aufnahme 0..100
  /// Mikrofon-Verstärkung beim Teach. Wird sie später geändert, gleicht die
  /// Erkennung die Pegel rechnerisch aus.
  float micGain = cfg::kMicGainDefault;

  /// true = Standardprofil ohne Teach (nur Pegel + Gleichmäßigkeit).
  bool isDefault() const { return valid && recordings == 0; }
};

/**
 * Gelernte Störgeräusche (z. B. Milchaufschäumer). Erkennt die KI eines davon,
 * wird es ignoriert. Je Eintrag nur die Pegel seiner Aufnahme (Anzeige) – die
 * Klangbilder liegen in den Trainingsdaten (Art Noise, Index = Position).
 */
struct NoiseProfile {
  struct Entry {
    float bgRmsDb = -90.0f;               ///< Hintergrundpegel bei der Aufnahme
    float levelDb = -90.0f;               ///< typischer (leiser) Pegel des Geräuschs
    float micGain = cfg::kMicGainDefault; ///< Mikrofon-Verstärkung bei der Aufnahme
  };
  uint8_t count = 0;
  Entry entries[cfg::kMaxNoises];
};

/**
 * Gelernte Mühlen – getrennt von den Störgeräuschen. Werden bei der
 * Pumpenerkennung ignoriert und lösen die Mühlen-Tara aus. Ihr Pegel ist
 * zusätzlich Bedingung: Erst ab der Mitte zwischen eigenem Hintergrund und
 * Mahlpegel gilt eine Mühle als laufend.
 */
struct GrinderProfile {
  static constexpr size_t kNameLen = 16;
  uint8_t count = 0;
  NoiseProfile::Entry entries[cfg::kMaxGrinders];
  char names[cfg::kMaxGrinders][kNameLen] = {};  ///< frei wählbar (UTF-8)
};

/// Vom Benutzer einstellbare Erkennungsparameter.
struct DetectorSettings {
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
  GrinderProfile grinders;
};

/// WLAN-Zugangsdaten.
struct WifiSettings {
  String ssid;
  String password;
};

/// Setzt ein Objekt auf seine Standardwerte zurück, ohne ein Temporärobjekt
/// auf dem Stack anzulegen.
template <typename T>
void resetInPlace(T& v) {
  v.~T();
  new (&v) T();
}

/// Laden / Speichern aller persistenten Daten.
namespace settings {

/// Öffnet NVS und Dateisystem.
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

uint8_t loadMicGain();
void saveMicGain(uint8_t gain);

void loadWifi(WifiSettings& w);
void saveWifi(const WifiSettings& w);

/// Plausibilisiert Einstellungen (z. B. aus der Webseite) auf gültige Bereiche.
void clamp(DetectorSettings& s);

}  // namespace settings
