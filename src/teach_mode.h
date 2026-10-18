// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file teach_mode.h
 * @brief Teach-Modus: Lernen von Pumpe, Mühle und Störgeräuschen.
 *
 * Die Modi sind einzeln wählbar und völlig unabhängig voneinander: Jede
 * Aufnahme beginnt mit ihrer eigenen Ruhe (3 s) und speichert eigenen
 * Hintergrund, Pegel und Mikrofon-Verstärkung. Die Reihenfolge des
 * Einlernens spielt keine Rolle. Die Klangbilder der Aufnahme (und ihrer
 * Ruhe) übernimmt die KI-Erkennung als Trainingsdaten (trainingFrames()).
 *
 * Ablauf:
 *   1. Background  – Gerät AUS, Hintergrundgeräusch wird aufgenommen (3 s)
 *   2. WaitPump    – Benutzer startet Bezug / Mühle / Störgeräusch; gewartet
 *                    wird auf einen deutlichen Pegelanstieg (max. 20 s)
 *   3. RecordPump  – Aufnahme, solange es hörbar ist. Pumpe: Ende nach 4 s
 *                    Stille (Pre-Infusion wird überbrückt), per "Fertig" oder
 *                    nach 60 s; Mühle/Störgeräusch: nach 1,5 s Stille (max. 30 s)
 *   4. Done        – Auswertung:
 *                    - Pegel (Hintergrund, leiser Teil), Gleichmäßigkeit, Pausen
 *                    - Bewertung OK / Warnung / Fehler mit Begründung
 *                    - was die bisherige KI in der Aufnahme hört
 *                    Ergebnis kann ersetzen (Pumpe), als neuer Eintrag
 *                    gespeichert (Mühle/Störgeräusch) oder ergänzt werden.
 */
#pragma once

#include <vector>

#include "audio_features.h"
#include "classifier.h"
#include "settings.h"

enum class TeachPhase { Idle, Background, WaitPump, RecordPump, Done };

enum class TeachMode { Pump, Noise, Grinder };

/// Mühle und Störgeräusch werden gleich aufgenommen und ausgewertet.
inline bool isNoiseMode(TeachMode m) { return m == TeachMode::Noise || m == TeachMode::Grinder; }

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
  float pumpSteadiness = 0.0f;   ///< Pegelschwankung der Pumpe in dB (95 %-Perzentil)
  uint16_t trainFrames = 0;      ///< Klangbilder dieser Aufnahme für die KI

  // Was die bisherige KI in der Aufnahme hört (vor dem Speichern)
  bool clsReady = false;         ///< KI war schon trainiert
  float clsAsPump = 0.0f;        ///< Anteil, der als Pumpe erkannt würde (Erkennung wie im Betrieb)
  int8_t grinderMatch = -1;      ///< Mühle: von der KI am häufigsten erkannte Mühle (-1 = keine)
  float grinderMatchShare = 0.0f;///< Anteil der Klangbilder, die dieser Mühle zugeordnet werden
  int8_t noiseMatch = -1;        ///< Störgeräusch: dito
  float noiseMatchShare = 0.0f;

  // Aus der Aufnahme berechnete Einstellungen (werden beim Speichern übernommen)
  uint16_t suggestedOffDelayMs = 700;

  char messages[kMaxMessages][kMessageLen] = {};  ///< Begründungen (UTF-8)
  uint8_t messageCount = 0;
  PumpProfile profile;           ///< Pumpe: gelernte Pegel (nur bei !Error speicherbar)
  NoiseProfile::Entry noise;     ///< Mühle / Störgeräusch: gelernte Pegel

  /// Fügt eine formatierte Meldung hinzu (printf-Syntax).
  void addMessage(const char* fmt, ...);
};

class TeachController {
 public:
  /// Reserviert die Aufnahmepuffer im PSRAM. Einmal beim Start aufrufen.
  void begin();

  /**
   * Startet einen neuen Teach-Durchlauf.
   * @param mode     Pumpe, Mühle oder Störgeräusch
   * @param current  aktuelle Einstellungen
   * @param profile  aktuelles Pumpenprofil (Bezugsgröße für "würde als Pumpe erkannt")
   * @param grinders bereits gelernte Mühlen (Zeiger bleibt gültig, für Namen und Liste)
   * @param noiseCount bereits gelernte Störgeräusche
   * @param cls      bisherige KI (Zeiger bleibt gültig; was hört sie in der Aufnahme?)
   * @param gain     aktuelle Mikrofon-Verstärkung (wird mit der Aufnahme gespeichert)
   */
  void start(uint32_t nowMs, TeachMode mode, const DetectorSettings& current, const PumpProfile& profile,
             const GrinderProfile* grinders, uint8_t noiseCount, const classifier::Classifier* cls, float gain);

  /// Bricht ab und kehrt in den Ruhezustand zurück.
  void cancel();

  /// Beendet die Aufnahme vorzeitig ("Fertig"); ausgewertet wird im nächsten Frame.
  void finish() { finishRequested_ = true; }

  /// Verarbeitet einen Audio-Frame (nur in aktiven Phasen wirksam).
  void process(const AudioFrame& f);

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
  /// Kurzer Name des Modus (JSON: "pump", "noise", "grinder").
  static const char* modeName(TeachMode m);
  /// Modus aus dem JSON-Namen (unbekannt -> Pumpe).
  static TeachMode modeFromName(const char* name);

  const TeachResult& result() const { return result_; }

  /**
   * Klangbilder der letzten Aufnahme für den Klassifikator (quantisierte
   * Klangfarbe, gleichmäßig auf höchstens maxFrames ausgedünnt).
   * @param background  true = Ruhe-Phase (Schritt 1), false = hörbarer Teil der Aufnahme
   */
  void trainingFrames(bool background, std::vector<int8_t>& out, size_t maxFrames) const;

  /// Aktueller Pegel und gemessener Hintergrundpegel in dBFS.
  float liveRmsDb() const { return liveRmsDb_; }
  float backgroundRmsDb() const { return bgRmsDb_; }

 private:
  void enterPhase(TeachPhase p, uint32_t nowMs);
  void finishBackground();
  void analyzePump();
  void analyzeNoise();
  /// Was hört die bisherige KI in den hörbaren Frames? (füllt clsReady, Treffer je Mühle/Störgeräusch)
  void classifyRecording(const std::vector<size_t>& active);
  void fail(const char* fmt, ...);
  void clearResult();

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
  float gain_ = cfg::kMicGainDefault;  ///< Mikrofon-Verstärkung der laufenden Aufnahme

  // Zustand beim Start (Kopien, da sich die Originale ändern können)
  DetectorSettings settings_;
  PumpProfile currentProfile_;
  const GrinderProfile* grinders_ = nullptr;
  uint8_t noiseCount_ = 0;
  const classifier::Classifier* classifier_ = nullptr;

  // Aufnahmepuffer (PSRAM)
  AudioFrame* bgFrames_ = nullptr;
  size_t bgCount_ = 0;
  AudioFrame* pumpFrames_ = nullptr;
  size_t pumpCount_ = 0;

  TeachResult result_;
};
