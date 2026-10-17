// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file shot_timer.h
 * @brief Bezugs-Timer und Historie der letzten Bezüge.
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "config.h"

/// Ein abgeschlossener Bezug.
struct ShotRecord {
  uint32_t durationMs = 0;  ///< Pumpenlaufzeit
  time_t endEpoch = 0;      ///< Uhrzeit des Endes (0 = unbekannt, kein NTP)
  float weightG = NAN;      ///< Gewicht in der Tasse (Waage), NAN = keine Waage
  float doseG = NAN;        ///< gewogenes Kaffeemehl (Dosier-Tara), NAN = unbekannt
};

class ShotTimer {
 public:
  /// Startet den Timer mit (ggf. rückdatiertem) Startzeitpunkt in millis().
  void start(uint32_t startMs);

  /**
   * Stoppt den Timer.
   * @param endMs           Endzeitpunkt in millis()
   * @param minHistoryMs    nur Bezüge ab dieser Dauer landen in der Historie
   * @return true, wenn der Timer lief
   */
  bool stop(uint32_t endMs, uint32_t minHistoryMs);

  /// true, wenn der letzte stop() einen Historien-Eintrag angelegt hat.
  bool lastStopRecorded() const { return lastStopRecorded_; }

  /// Aktualisiert das Gewicht des neuesten Historien-Eintrags (Nachtropfen).
  void setLatestWeight(float g);
  /// Setzt die Kaffeemehl-Dosis des neuesten Historien-Eintrags.
  void setLatestDose(float g);

  /// Setzt die Anzeige auf 0 (ohne Historien-Eintrag).
  void reset();

  /// Löscht die Historie.
  void clearHistory();

  bool running() const { return running_; }

  /// Aktuell anzuzeigende Zeit: laufend -> seit Start, sonst letzter Bezug.
  uint32_t displayMs(uint32_t nowMs) const;

  /// Anzahl der Historien-Einträge.
  size_t historyCount() const { return historyCount_; }

  /// Historien-Eintrag, Index 0 = neuester.
  const ShotRecord& history(size_t i) const;

 private:
  bool running_ = false;
  bool lastStopRecorded_ = false;
  uint32_t startMs_ = 0;
  uint32_t lastDurationMs_ = 0;
  ShotRecord history_[cfg::kHistorySize];  ///< Ringpuffer
  size_t historyHead_ = 0;                 ///< nächste Schreibposition
  size_t historyCount_ = 0;
};
