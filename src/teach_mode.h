/**
 * @file teach_mode.h
 * @brief Teach-Modus: Lernen des Pumpengeräuschs bzw. von Störgeräuschen.
 *
 * Modus "Pumpe":
 *   1. Background  – Pumpe AUS, Hintergrundgeräusch wird aufgenommen (3 s)
 *   2. WaitPump    – Benutzer startet einen Bezug; gewartet wird auf einen
 *                    deutlichen Pegelanstieg (max. 20 s)
 *   3. RecordPump  – Aufnahme läuft, solange die Pumpe läuft. Ende automatisch,
 *                    wenn die Pumpe 4 s still ist, per "Fertig" oder nach 60 s.
 *   4. Done        – Auswertung:
 *                    - bis zu 3 Klangmuster (Anlauf/Druckaufbau/voller Druck)
 *                    - Pumpentakt und Rhythmus-Fingerabdruck (Autokorrelation)
 *                    - passende Schwellen werden aus der Aufnahme berechnet
 *                    - Bewertung OK / Warnung / Fehler mit Begründung, u. a.
 *                      welcher Anteil der Aufnahme damit erkannt würde
 *                    Ergebnis kann das Profil ersetzen oder ergänzen.
 *
 * Modus "Störgeräusch" (z. B. Kaffeemühle; benötigt ein Pumpenprofil):
 *   2. WaitPump    – Benutzer schaltet das Störgeräusch ein
 *   3. RecordPump  – Aufnahme, bis es 1,5 s still ist (max. 30 s)
 *   4. Done        – Klangmuster + Rhythmus des Störgeräuschs; Anzeige, wie oft
 *                    es bisher als Pumpe erkannt wurde und wie oft danach.
 */
#pragma once

#include "audio_features.h"
#include "settings.h"

enum class TeachPhase { Idle, Background, WaitPump, RecordPump, Done };

enum class TeachMode { Pump, Noise };

enum class TeachQuality { Ok, Warning, Error };

/// Ergebnis einer Teach-Aufnahme.
struct TeachResult {
  static constexpr size_t kMaxMessages = 8;
  static constexpr size_t kMessageLen = 160;

  TeachMode mode = TeachMode::Pump;
  TeachQuality quality = TeachQuality::Error;
  int score = 0;                 ///< Gesamtbewertung 0..100

  // Messwerte
  float snrDb = 0.0f;            ///< leiser Teil (10 %) über Hintergrund
  float bgStdDb = 0.0f;          ///< Schwankung des Hintergrunds
  float clipRatio = 0.0f;        ///< Anteil übersteuerter Samples
  float activeSeconds = 0.0f;    ///< hörbare Laufzeit in der Aufnahme
  uint32_t longestPauseMs = 0;   ///< längste Pause (z. B. Pre-Infusion)
  float coverage = 0.0f;         ///< Anteil der Pumpen-Frames, die erkannt würden
  float falseRatio = 0.0f;       ///< Anteil der Hintergrund-Frames, die auslösen würden
  float rejectedRatio = 0.0f;    ///< Anteil der Pumpe, der als Störgeräusch verworfen würde
  float selfSimilarity = 0.0f;   ///< Klangmuster-Ähnlichkeit der Pumpe (5 %-Perzentil)
  float bgSimilarity = 0.0f;     ///< Klangmuster-Ähnlichkeit des Hintergrunds (90 %)
  float pumpPeriodicity = 0.0f;  ///< Takt-Stärke der Pumpe (10 %-Perzentil)
  float bgPeriodicity = 0.0f;    ///< Takt-Stärke des Hintergrunds (90 %-Perzentil)
  float pumpRhythm = 0.0f;       ///< Rhythmus-Form der Pumpe (5 %-Perzentil)
  float bgRhythm = 0.0f;         ///< Rhythmus-Form des Hintergrunds (90 %-Perzentil)
  float pumpSteadiness = 0.0f;   ///< Pegelschwankung der Pumpe in dB (95 %-Perzentil)
  float contrastDb = 0.0f;       ///< Ausprägung der Klangfarbe (Streuung über die Bänder)
  int distinctBands = 0;         ///< Bänder mit deutlichem Pumpenanteil

  // Störgeräusch-Modus
  float noiseHitBefore = 0.0f;   ///< Anteil, der bisher als Pumpe erkannt wurde
  float noiseHitAfter = 0.0f;    ///< Anteil mit gespeichertem Störgeräusch
  NoiseProfile::Entry noise;     ///< gelerntes Störgeräusch

  // Aus der Aufnahme berechnete Einstellungen (werden beim Speichern übernommen)
  float suggestedSimilarity = 0.75f;
  float suggestedPeriodicity = 0.4f;
  uint16_t suggestedOffDelayMs = 700;

  char messages[kMaxMessages][kMessageLen] = {};  ///< Begründungen (UTF-8)
  uint8_t messageCount = 0;
  PumpProfile profile;           ///< gelerntes Profil (nur bei !Error speicherbar)

  /// Fügt eine formatierte Meldung hinzu (printf-Syntax).
  void addMessage(const char* fmt, ...);
};

class TeachController {
 public:
  /// Reserviert die Aufnahmepuffer im PSRAM. Einmal beim Start aufrufen.
  void begin();

  /**
   * Startet einen neuen Teach-Durchlauf.
   * @param mode     Pumpe oder Störgeräusch
   * @param current  aktuelle Einstellungen
   * @param profile  aktuelles Pumpenprofil (Störgeräusch-Modus: Bezugsgröße)
   * @param noise    bereits gelernte Störgeräusche (für die Bewertung)
   */
  void start(uint32_t nowMs, TeachMode mode, const DetectorSettings& current,
             const PumpProfile& profile, const NoiseProfile& noise);

  /// Bricht ab und kehrt in den Ruhezustand zurück.
  void cancel();

  /// Beendet die Aufnahme vorzeitig ("Fertig"); ausgewertet wird im nächsten Frame.
  void finish() { finishRequested_ = true; }

  /**
   * Verarbeitet einen Audio-Frame (nur in aktiven Phasen wirksam).
   * @param curve  Rhythmus-Kurve des Frames (FeatureExtractor::acfCurve())
   */
  void process(const AudioFrame& f, const float* curve);

  TeachPhase phase() const { return phase_; }
  TeachMode mode() const { return mode_; }

  /// true während Aufnahme oder Ergebnisanzeige.
  bool engaged() const { return phase_ != TeachPhase::Idle; }

  /// true während einer aktiven Aufnahmephase.
  bool recording() const {
    return phase_ == TeachPhase::Background || phase_ == TeachPhase::WaitPump ||
           phase_ == TeachPhase::RecordPump;
  }

  /// Fortschritt der aktuellen Phase 0..1.
  float progress() const { return progress_; }

  /// Bisherige Dauer der Aufnahme (Phase RecordPump) in ms.
  uint32_t recordedMs() const { return recordedMs_; }

  /// Kurze Anweisung für den Benutzer zur aktuellen Phase.
  const char* instruction() const;

  /// Kurzer Name der Phase (für die Webseite / JSON).
  static const char* phaseName(TeachPhase p);

  const TeachResult& result() const { return result_; }

  /// Aktueller Pegel und gemessener Hintergrundpegel in dBFS.
  float liveRmsDb() const { return liveRmsDb_; }
  float backgroundRmsDb() const { return bgRmsDb_; }

 private:
  void enterPhase(TeachPhase p, uint32_t nowMs);
  void finishBackground();
  void analyzePump();
  void analyzeNoise();
  void fail(const char* fmt, ...);
  void clearResult();

  /// Speichert eine Rhythmus-Kurve kompakt als int8 (spart PSRAM).
  static void storeCurve(int8_t* dst, const float* curve);
  /// Entpackt eine gespeicherte Rhythmus-Kurve.
  static void loadCurve(const int8_t* src, float* curve);

  TeachMode mode_ = TeachMode::Pump;
  TeachPhase phase_ = TeachPhase::Idle;
  uint32_t phaseStartMs_ = 0;
  float progress_ = 0.0f;
  float liveRmsDb_ = -90.0f;
  float bgRmsDb_ = -90.0f;
  uint32_t triggerFrames_ = 0;   ///< Zähler für den Pegelanstieg in WaitPump
  uint32_t lastLoudMs_ = 0;      ///< letzter Frame mit hörbarem Geräusch
  size_t lastLoudIndex_ = 0;     ///< dessen Index in pumpFrames_ (+1)
  uint32_t recordedMs_ = 0;
  volatile bool finishRequested_ = false;

  // Zustand beim Start (Kopien, da sich die Originale ändern können)
  DetectorSettings settings_;
  PumpProfile currentProfile_;
  NoiseProfile currentNoise_;

  // Aufnahmepuffer (PSRAM)
  AudioFrame* bgFrames_ = nullptr;
  int8_t* bgCurves_ = nullptr;
  size_t bgCount_ = 0;
  AudioFrame* pumpFrames_ = nullptr;
  int8_t* pumpCurves_ = nullptr;
  size_t pumpCount_ = 0;

  TeachResult result_;
};
