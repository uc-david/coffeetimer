// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file pump_detector.h
 * @brief Erkennt das Ein- und Ausschalten der Pumpe anhand des gelernten Profils.
 *
 * Merkmale je Frame (siehe score()):
 *  - similarity:   Klangfarbe – Korrelation des Anteils über dem Hintergrund
 *                  (mittelwertfrei, d. h. nur welche Bänder relativ stärker
 *                  sind) mit dem besten gelernten Muster (0..1)
 *  - periodicity:  Takt-Stärke – Rhythmus-Kurve beim gelernten Pumpentakt (0..1)
 *  - rhythm:       Rhythmus-Form – Korrelation der ganzen Rhythmus-Kurve mit
 *                  dem gelernten Fingerabdruck (-1..1)
 *  - levelRatio:   Pegel zwischen Hintergrund (0) und leisem Teil der Pumpe (1)
 *  - steadiness:   Pegelschwankung (dB) der letzten ~0,5 s der lauten Phase
 *  - noise*:       dieselben Vergleiche gegen gelernte Störgeräusche
 *
 * Entscheidung (siehe matches()):
 *  - Pegel ausreichend UND gleichmäßig (Pumpe läuft ruhig, Niesen/Sprache nicht) UND
 *  - Pumpe mit Takt (Vibrationspumpe): Takt-Stärke UND Rhythmus-Form passen.
 *    Das Klangbild ändert sich unter Druck stark, der Rhythmus nicht. Die
 *    Rhythmus-Form unterscheidet die Pumpe (ein Schlag je Netzperiode) von
 *    netzbrummenden Motoren wie einer Kaffeemühle.
 *  - Pumpe ohne Takt (Rotationspumpe): Klangmuster passt.
 *  - UND das Geräusch ist der Pumpe ähnlicher als jedem gelernten Störgeräusch.
 *
 * Danach Entprellung mit Ein-/Ausschaltverzögerung und Hysterese, damit kurze
 * Störgeräusche oder Aussetzer den Timer nicht beeinflussen.
 */
#pragma once

#include "audio_features.h"
#include "classifier.h"
#include "settings.h"

/// Vergleichswerte eines Frames mit Pumpen- und Störgeräusch-Profil.
struct FrameScores {
  float similarity = 0.0f;       ///< Klangmuster-Ähnlichkeit zur Pumpe 0..1
  float periodicity = 0.0f;      ///< Takt-Stärke beim Pumpentakt 0..1
  float rhythm = 0.0f;           ///< Ähnlichkeit der Rhythmus-Form zur Pumpe -1..1
  float levelRatio = 0.0f;       ///< Pegel relativ Hintergrund(0) .. Pumpe(1)
  /// Ähnlichstes Störgeräusch ODER Mühle (beides wird bei der Pumpe ignoriert)
  float noiseSimilarity = 0.0f;  ///< bestes Klangmuster
  float noiseRhythm = -1.0f;     ///< beste Rhythmus-Form
  bool hasNoise = false;         ///< mindestens ein Störgeräusch / eine Mühle verglichen
  /// Ähnlichste Mühle
  float grinderSimilarity = 0.0f;
  float grinderRhythm = -1.0f;
  int8_t grinderIndex = -1;      ///< -1 = keine Mühle gelernt
  float grinderLevelRatio = 0.0f; ///< Pegel relativ zum Hintergrund (0) .. Mahlpegel (1) der Mühle
  bool bestIsGrinder = false;    ///< eine Mühle passt besser als alle Störgeräusche
  float steadiness = 99.0f;      ///< Pegelschwankung in dB (klein = gleichmäßig)
  /// KI-Erkennung (Klassifikator): entscheidet statt der Klangmuster, was zu hören ist
  bool clsActive = false;
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
  bool rejected = false;     ///< klang nach Pumpe, aber eher nach Störgeräusch
  bool grinderOn = false;    ///< Mühle läuft (entprellt)
  int8_t grinderIndex = -1;  ///< welche Mühle (solange grinderOn)
  bool periodic = false;     ///< Takt wird für die Erkennung genutzt
  FrameScores scores;
  float rmsDb = -90.0f;      ///< aktueller Pegel dBFS
  float bgDb = -90.0f;       ///< verwendeter Hintergrundpegel (Standardprofil: automatisch)
};

class PumpDetector {
 public:
  enum class Event { None, Started, Stopped };

  void setProfile(const PumpProfile& p);
  void setNoise(const NoiseProfile& n) { noise_ = n; }
  void setSettings(const DetectorSettings& s) { settings_ = s; }
  /// Gelernte Mühlen (Zeiger bleibt gültig, Zugriff nur unter dem App-Mutex).
  void setGrinders(const GrinderProfile* g) { grinders_ = g; }
  /// KI-Erkennung: trainierter Klassifikator (Zeiger bleibt gültig) und ob er genutzt wird.
  void setClassifier(const classifier::Classifier* c, bool enabled) {
    classifier_ = c;
    useClassifier_ = enabled;
  }
  /// Aktuelle Mikrofon-Verstärkung: Frames kommen unverändert, jedes Geräusch
  /// rechnet auf die Verstärkung seiner eigenen Aufnahme um.
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
   * @param curve  Rhythmus-Kurve des Frames (FeatureExtractor::acfCurve())
   * @return Started / Stopped beim Zustandswechsel, sonst None.
   */
  Event process(const AudioFrame& f, const float* curve);

  const DetectorStatus& status() const { return status_; }

  /// Geschätzter Startzeitpunkt (millis) der Pumpe – erster aktiver Frame.
  uint32_t onsetMs() const { return onsetMs_; }

  /// Zeitpunkt (millis) des letzten aktiven Frames – Ende des Bezugs.
  uint32_t lastActiveMs() const { return lastActiveMs_; }

  // --- Gemeinsame Berechnungen (auch vom Teach-Modus genutzt) ---------------

  /// Berechnet alle Vergleichswerte eines Frames.
  /// @param steadiness  Ergebnis von SteadinessTracker::update()
  /// @param g           gelernte Mühlen (optional)
  /// @param gain        Mikrofon-Verstärkung, mit der bandDb/rmsDb aufgenommen wurden
  static FrameScores score(const float* bandDb, float rmsDb, const float* curve, float steadiness,
                           const PumpProfile& p, const NoiseProfile& n,
                           const GrinderProfile* g, float gain);

  /// Pegelkorrektur in dB, um eine Aufnahme mit currentGain auf learnedGain umzurechnen.
  static float gainOffsetDb(float learnedGain, float currentGain);

  /// Gilt der Pegel als "laut" (Pumpe möglich)? Grundlage der Gleichmäßigkeit.
  static bool isLoud(float rmsDb, const PumpProfile& p, const DetectorSettings& st, bool hysteresis);

  /**
   * Entscheidet, ob ein Frame nach Pumpe klingt.
   * @param hysteresis  true = Pumpe läuft bereits (etwas tolerantere Schwellen)
   * @param rejected    optional: true, wenn nur wegen Störgeräusch abgelehnt
   */
  static bool matches(const FrameScores& s, const PumpProfile& p, const DetectorSettings& st,
                      bool hysteresis, bool* rejected = nullptr);

  /**
   * Anteil eines Spektrums über dem Hintergrund, mittelwertfrei und auf Länge 1
   * normiert. Ohne den Mittelwert ähneln sich alle lauten, breitbandigen
   * Geräusche (alle Bänder erhöht) – mit ihm zählt nur die Klangfarbe.
   * @return Länge vor der Normierung (≈0: nichts über dem Hintergrund)
   */
  static float excessVector(const float* bandDb, const float* bgBandDb, float* out);

  /// Beste Kosinus-Ähnlichkeit eines Überschuss-Vektors zu einer Musterliste.
  static float bestTemplate(const float* excess, const float (*templates)[cfg::kNumBands],
                            size_t count);

  /// Pegel relativ zwischen Hintergrund (0) und Pumpen-Referenz (1).
  static float levelRatio(float rmsDb, const PumpProfile& p);

  /// Macht eine Rhythmus-Kurve mittelwertfrei mit Länge 1 (für Korrelation).
  static void normalizeCurve(const float* in, float* out);

  /// Korrelation einer Rhythmus-Kurve mit einem normierten Fingerabdruck (-1..1).
  static float rhythmSimilarity(const float* curve, const float* normalizedTemplate);

  /// Takt-Stärke: Maximum der Rhythmus-Kurve um lag (± cfg::kLagTolerance).
  static float periodicityAt(const float* curve, float lag);

  /**
   * Ergänzt ein bestehendes Profil um eine weitere Teach-Aufnahme
   * (z. B. echter Bezug zusätzlich zum Leerbezug).
   * @param note  erhält einen Hinweis, falls der Takt nicht übernommen werden konnte
   */
  static void mergeProfile(PumpProfile& into, const PumpProfile& from, String& note);

  /// Ergänzt ein Störgeräusch um eine weitere Aufnahme (z. B. Mühle leer / mit Bohnen).
  static void mergeNoise(NoiseProfile::Entry& into, const NoiseProfile::Entry& from);

 private:
  PumpProfile profile_;
  NoiseProfile noise_;
  DetectorSettings settings_;
  DetectorStatus status_;
  /// Standardprofil: Raumpegel automatisch nachführen.
  void updateAutoBackground(float rmsDb);
  /// Mühle erkennen: lautes Geräusch, das dem Mühlen-Störgeräusch ähnlicher ist als der Pumpe.
  void updateGrinder(uint32_t now, float rawRmsDb);
  const classifier::Classifier* classifier_ = nullptr;
  bool useClassifier_ = false;

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
