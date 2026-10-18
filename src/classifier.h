// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file classifier.h
 * @brief Kleiner Klassifikator, direkt auf dem Gerät trainiert (lineare
 *        Diskriminanzanalyse, LDA).
 *
 * Die Klangmuster-Erkennung lernt jedes Geräusch für sich. Der Klassifikator
 * lernt dagegen aus ALLEN Aufnahmen eines Profils gemeinsam, worin sie sich
 * unterscheiden (welche Frequenzbänder Pumpe, Mühlen, Störgeräusche und Ruhe
 * trennen) und gewichtet genau diese Bänder.
 *
 * Merkmal je Messfenster: Klangfarbe = 40 Bandpegel minus ihrem Mittelwert
 * (unabhängig von Lautstärke und Mikrofon-Verstärkung).
 *
 * Trainingsdaten: Bei jedem Teach werden bis zu kMaxFramesPerRecord
 * Klangbilder gespeichert (int8, 0,5 dB Auflösung) – je Profil in einer eigenen
 * Datei (settings::loadTraining / saveTraining).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "config.h"

namespace classifier {

/// Art einer Klasse.
enum class Kind : uint8_t { Pump = 0, Grinder = 1, Noise = 2, Background = 3 };

constexpr size_t kMaxFramesPerRecord = 200;  ///< Klangbilder je Aufnahme
constexpr size_t kMaxClasses = 1 + cfg::kMaxGrinders + cfg::kMaxNoises + 1;
constexpr float kQuantStep = 0.5f;           ///< dB je int8-Stufe

/// Klangbilder einer Teach-Aufnahme mit ihrer Klasse.
struct Record {
  Kind kind = Kind::Pump;
  uint8_t index = 0;            ///< Mühle/Störgeräusch: Position in der Liste
  std::vector<int8_t> frames;   ///< n × cfg::kNumBands, Klangfarbe in 0,5-dB-Stufen
  size_t count() const { return frames.size() / cfg::kNumBands; }
};

/// Alle Trainingsdaten eines Profils.
struct TrainingSet {
  std::vector<Record> records;

  /// Hängt eine Aufnahme an; je Klasse werden höchstens maxPerClass Aufnahmen
  /// behalten (die ältesten fallen weg).
  void add(Record&& r, size_t maxPerClass);
  /// Entfernt alle Aufnahmen einer Art (z. B. Pumpe bei "Ersetzen").
  void removeKind(Kind kind);
  /// Entfernt die Aufnahmen eines Listeneintrags und rückt die Nummern der
  /// folgenden nach (wie beim Löschen aus der Mühlen-/Störgeräusch-Liste).
  void removeEntry(Kind kind, uint8_t index);
  size_t totalFrames() const;
};

/// Klangfarbe eines Messfensters (Bandpegel minus Mittelwert), quantisiert.
void quantizeShape(const float* bandDb, int8_t* out);

/// Ergebnis einer Klassifikation.
struct Result {
  bool valid = false;
  Kind kind = Kind::Background;  ///< wahrscheinlichste Klasse
  uint8_t index = 0;
  float probability = 0.0f;      ///< deren Wahrscheinlichkeit 0..1
  float pumpProbability = 0.0f;  ///< Wahrscheinlichkeit "Pumpe"
};

class Classifier {
 public:
  /**
   * Trainiert aus den Trainingsdaten. Benötigt Pumpe und mindestens eine
   * weitere Klasse (z. B. Ruhe), sonst bleibt der Klassifikator unbenutzt.
   * @return true, wenn einsatzbereit
   */
  bool train(const TrainingSet& set);

  bool ready() const { return ready_; }
  size_t classCount() const { return classCount_; }
  size_t frameCount() const { return frames_; }
  /// Anteil der Trainings-Klangbilder, die richtig erkannt werden (0..1).
  float trainingAccuracy() const { return accuracy_; }

  /// Klassifiziert ein Messfenster (Bandpegel in dB).
  Result classify(const float* bandDb) const;

 private:
  struct ClassInfo {
    Kind kind;
    uint8_t index;
    float w[cfg::kNumBands];  ///< Gewichte (Σ⁻¹·μ)
    float b;                  ///< Konstante (-½·μᵀΣ⁻¹μ)
  };
  /// Diskriminanzwerte aller Klassen für ein Merkmal (float, schon entquantisiert).
  void scores(const float* x, float* out) const;
  Result pick(const float* score) const;

  ClassInfo classes_[kMaxClasses];
  size_t classCount_ = 0;
  size_t frames_ = 0;
  float accuracy_ = 0.0f;
  bool ready_ = false;
};

const char* kindName(Kind k);

}  // namespace classifier
