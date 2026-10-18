// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file pump_detector.h
 * @brief Erkennt das Ein- und Ausschalten der Pumpe.
 *
 * Merkmale je Frame (siehe score()):
 *  - levelRatio:  Pegel zwischen Hintergrund (0) und leisem Teil der Pumpe (1)
 *  - steadiness:  Pegelschwankung (dB) der letzten ~0,5 s der lauten Phase
 *  - cls*:        KI-Erkennung – was ist zu hören (Pumpe, Mühle k,
 *                 Störgeräusch k, Ruhe) und wie sicher
 *
 * Entscheidung (siehe matches()):
 *  - Pegel ausreichend UND gleichmäßig (Pumpe läuft ruhig, Niesen/Sprache nicht) UND
 *  - die KI erkennt die Pumpe (ist sie noch nicht eingelernt: jedes laute,
 *    gleichmäßige Geräusch, außer die KI erkennt eine Mühle / ein Störgeräusch).
 *
 * Danach Entprellung mit Ein-/Ausschaltverzögerung und Hysterese, damit kurze
 * Störgeräusche oder Aussetzer den Timer nicht beeinflussen.
 */
#pragma once

#include "audio_features.h"
#include "classifier.h"
#include "settings.h"

/// Bewertung eines Frames.
struct FrameScores {
  float levelRatio = 0.0f;       ///< Pegel relativ Hintergrund(0) .. Pumpe(1)
  float steadiness = 99.0f;      ///< Pegelschwankung in dB (klein = gleichmäßig)
  /// KI-Erkennung (Klassifikator)
  bool clsActive = false;        ///< trainiert und ausgewertet
  bool clsPumpKnown = false;     ///< die Pumpe ist eingelernt
  classifier::Kind clsKind = classifier::Kind::Background;
  uint8_t clsIndex = 0;
  float clsProb = 0.0f;          ///< Wahrscheinlichkeit der erkannten Klasse
  float clsPump = 0.0f;          ///< Wahrscheinlichkeit "Pumpe"
};

/**
 * Misst die Gleichmäßigkeit des Pegels: Standardabweichung (dB) über die
 * letzten cfg::kSteadyFrames Frames der aktuellen lauten Phase. Vor
 * cfg::kSteadyMinFrames lauten Frames gilt das Geräusch als (noch) unruhig.
 */
class SteadinessTracker {
 public:
  static constexpr float kUnsteady = 99.0f;

  void reset() { count_ = 0; head_ = 0; }

  /// @param loud  false beendet die laute Phase
  /// @return Standardabweichung in dB oder kUnsteady
  float update(float rmsDb, bool loud);

  /// Anzahl Frames der aktuellen lauten Phase.
  size_t runLength() const { return count_; }

 private:
  float buf_[cfg::kSteadyFrames] = {};
  size_t count_ = 0;  ///< Frames der lauten Phase (kann > Puffergröße sein)
  size_t head_ = 0;
};

/// Momentane Messwerte des Detektors (für Anzeige und Webseite).
struct DetectorStatus {
  bool pumpOn = false;       ///< entprellter Zustand
  bool rawActive = false;    ///< unentprellte Einzelentscheidung des Frames
  bool rejected = false;     ///< laut und gleichmäßig, aber als Mühle/Störgeräusch erkannt
  bool grinderOn = false;    ///< Mühle läuft (entprellt)
  int8_t grinderIndex = -1;  ///< welche Mühle (solange grinderOn)
  FrameScores scores;
  float rmsDb = -90.0f;      ///< aktueller Pegel dBFS
  float bgDb = -90.0f;       ///< verwendeter Hintergrundpegel (Standardprofil: automatisch)
};

class PumpDetector {
 public:
  enum class Event { None, Started, Stopped };

  void setProfile(const PumpProfile& p);
  void setSettings(const DetectorSettings& s) { settings_ = s; }
  /// Gelernte Mühlen (Zeiger bleibt gültig, Zugriff nur unter dem App-Mutex).
  void setGrinders(const GrinderProfile* g) { grinders_ = g; }
  /// Trainierter Klassifikator (Zeiger bleibt gültig, Zugriff nur unter dem App-Mutex).
  void setClassifier(const classifier::Classifier* c) { classifier_ = c; }
  /// Aktuelle Mikrofon-Verstärkung: Frames kommen unverändert, Pumpe und
  /// Mühlen rechnen auf die Verstärkung ihrer eigenen Aufnahme um.
  void setMicGain(float gain) { gain_ = gain; }

  /// Quelle der Pumpenerkennung (siehe vibration::Mode).
  enum class Source : uint8_t { Mic = 0, Vib = 1, Both = 2 };
  /**
   * Aktueller Zustand der Vibrationserkennung, vor jedem process() setzen.
   * @param sinceMs  Beginn der lauten Vibrationsphase (für die Rückdatierung)
   */
  void setVibration(Source source, bool active, uint32_t sinceMs) {
    source_ = source;
    vibActive_ = active;
    vibSinceMs_ = sinceMs;
  }

  /// Setzt den entprellten Zustand zurück (z. B. nach dem Teach-Modus).
  void reset();

  /**
   * Verarbeitet einen Frame.
   * @return Started / Stopped beim Zustandswechsel, sonst None.
   */
  Event process(const AudioFrame& f);

  const DetectorStatus& status() const { return status_; }

  /// Geschätzter Startzeitpunkt (millis) der Pumpe – erster aktiver Frame.
  uint32_t onsetMs() const { return onsetMs_; }

  /// Zeitpunkt (millis) des letzten aktiven Frames – Ende des Bezugs.
  uint32_t lastActiveMs() const { return lastActiveMs_; }

  // --- Gemeinsame Berechnungen (auch vom Teach-Modus genutzt) ---------------

  /**
   * Bewertet einen Frame.
   * @param steadiness  Ergebnis von SteadinessTracker::update()
   * @param gain        Mikrofon-Verstärkung, mit der bandDb/rmsDb aufgenommen wurden
   * @param cls         trainierter Klassifikator (optional)
   */
  static FrameScores score(const float* bandDb, float rmsDb, float steadiness, const PumpProfile& p,
                           float gain, const classifier::Classifier* cls);

  /// Pegelkorrektur in dB, um eine Aufnahme mit currentGain auf learnedGain umzurechnen.
  static float gainOffsetDb(float learnedGain, float currentGain);

  /// Gilt der Pegel als "laut" (Pumpe möglich)? Grundlage der Gleichmäßigkeit.
  static bool isLoud(float rmsDb, const PumpProfile& p, const DetectorSettings& st, bool hysteresis);

  /**
   * Entscheidet, ob ein Frame nach Pumpe klingt.
   * @param hysteresis  true = Pumpe läuft bereits (etwas tolerantere Schwellen)
   * @param rejected    optional: true, wenn die KI eine Mühle / ein Störgeräusch erkennt
   */
  static bool matches(const FrameScores& s, const PumpProfile& p, const DetectorSettings& st,
                      bool hysteresis, bool* rejected = nullptr);

  /// Pegel relativ zwischen Hintergrund (0) und Pumpen-Referenz (1).
  static float levelRatio(float rmsDb, const PumpProfile& p);

  /// Ergänzt das Pumpenprofil um die Pegel einer weiteren Teach-Aufnahme.
  static void mergeProfile(PumpProfile& into, const PumpProfile& from);

  /// Ergänzt eine Mühle / ein Störgeräusch um die Pegel einer weiteren Aufnahme.
  static void mergeNoise(NoiseProfile::Entry& into, const NoiseProfile::Entry& from);

 private:
  PumpProfile profile_;
  DetectorSettings settings_;
  DetectorStatus status_;
  /// Standardprofil: Raumpegel automatisch nachführen.
  void updateAutoBackground(float rmsDb);
  /// Mühle erkennen: die KI erkennt sie und sie ist laut genug.
  void updateGrinder(uint32_t now, float rawRmsDb);

  const classifier::Classifier* classifier_ = nullptr;
  const GrinderProfile* grinders_ = nullptr;
  float gain_ = cfg::kMicGainDefault;
  Source source_ = Source::Mic;
  bool vibActive_ = false;
  uint32_t vibSinceMs_ = 0;
  uint32_t grinderSinceMs_ = 0;    ///< Beginn der aktuellen Mühlen-Phase (0 = keine)
  uint32_t grinderLastMs_ = 0;     ///< letzter Frame, der nach Mühle klang

  SteadinessTracker steady_;
  bool autoBgInit_ = false;     ///< automatischer Hintergrund initialisiert?
  uint32_t loudSinceMs_ = 0;    ///< Beginn der aktuellen lauten Phase
  uint32_t activeSinceMs_ = 0;  ///< Beginn der aktuellen aktiven Phase (0 = keine)
  uint32_t onsetMs_ = 0;
  uint32_t lastActiveMs_ = 0;
};
