// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file pump_detector.cpp
 * @brief Implementierung der Pumpenerkennung.
 */
#include "pump_detector.h"

#include <math.h>

namespace {

/// Faktoren der Hysterese: Ist die Pumpe bereits EIN, genügen etwas
/// niedrigere Werte, um EIN zu bleiben. Verhindert Flattern an der Schwelle.
constexpr float kHystLevel = 0.70f;
constexpr float kHystSteady = 1.30f;  // Pegelschwankung darf bei laufender Pumpe etwas größer sein

/// Maximale Rückdatierung des Timer-Starts auf den Beginn der lauten Phase.
constexpr uint32_t kMaxBackdateMs = 1500;
// Mühle: so lange hörbar, bis sie als laufend gilt / Aussetzer, die toleriert
// werden / Stille, nach der sie als aus gilt.
constexpr uint32_t kGrinderOnMs = 1000;
constexpr uint32_t kGrinderGapMs = 300;
constexpr uint32_t kGrinderOffMs = 1500;
/// Mindestpegel der Mühle: Mitte zwischen ihrem Hintergrund und ihrem Mahlpegel.
constexpr float kGrinderLevel = 0.5f;

}  // namespace

// ---------------------------------------------------------------------------
// Gemeinsame Berechnungen
// ---------------------------------------------------------------------------

float SteadinessTracker::update(float rmsDb, bool loud) {
  if (!loud) {
    reset();
    return kUnsteady;
  }
  buf_[head_] = rmsDb;
  head_ = (head_ + 1) % cfg::kSteadyFrames;
  ++count_;
  if (count_ < cfg::kSteadyMinFrames) return kUnsteady;
  const size_t n = count_ < cfg::kSteadyFrames ? count_ : cfg::kSteadyFrames;
  float mean = 0.0f;
  for (size_t i = 0; i < n; ++i) mean += buf_[i];
  mean /= n;
  float var = 0.0f;
  for (size_t i = 0; i < n; ++i) var += (buf_[i] - mean) * (buf_[i] - mean);
  return sqrtf(var / n);
}

float PumpDetector::levelRatio(float rmsDb, const PumpProfile& p) {
  const float span = fmaxf(p.pumpRmsDb - p.bgRmsDb, 1.0f);
  return (rmsDb - p.bgRmsDb) / span;
}

bool PumpDetector::isLoud(float rmsDb, const PumpProfile& p, const DetectorSettings& st,
                          bool hysteresis) {
  return levelRatio(rmsDb, p) >= st.levelThreshold * (hysteresis ? kHystLevel : 1.0f);
}

float PumpDetector::gainOffsetDb(float learnedGain, float currentGain) {
  return learnedGain > 0.0f && currentGain > 0.0f ? 20.0f * log10f(learnedGain / currentGain) : 0.0f;
}

FrameScores PumpDetector::score(const float* bandDb, float rmsDb, float steadiness, const PumpProfile& p,
                                float gain, const classifier::Classifier* cls) {
  FrameScores s;
  s.steadiness = steadiness;
  s.levelRatio = levelRatio(rmsDb + gainOffsetDb(p.micGain, gain), p);
  // Die KI vergleicht nur die Klangfarbe (Bandpegel minus Mittelwert) –
  // unabhängig von Lautstärke und Mikrofon-Verstärkung.
  if (cls && cls->ready()) {
    const classifier::Result r = cls->classify(bandDb);
    s.clsActive = r.valid;
    s.clsPumpKnown = cls->hasKind(classifier::Kind::Pump);
    s.clsKind = r.kind;
    s.clsIndex = r.index;
    s.clsProb = r.probability;
    s.clsPump = r.pumpProbability;
  }
  return s;
}

bool PumpDetector::matches(const FrameScores& s, const PumpProfile& p, const DetectorSettings& st,
                           bool hysteresis, bool* rejected) {
  if (rejected) *rejected = false;
  if (!p.valid) return false;

  // Pegel und Gleichmäßigkeit: leise oder unruhige Geräusche kommen nie in Frage.
  const float lvlTh = st.levelThreshold * (hysteresis ? kHystLevel : 1.0f);
  const float steadyTh = p.steadyThreshold * (hysteresis ? kHystSteady : 1.0f);
  if (s.levelRatio < lvlTh || s.steadiness > steadyTh) return false;
  if (!s.clsActive) return true;  // noch nichts eingelernt: Standardprofil

  // WAS zu hören ist, entscheidet die KI.
  const float minProb = hysteresis ? cfg::kClsHoldProb : cfg::kClsMinProb;
  const bool other = s.clsKind == classifier::Kind::Grinder || s.clsKind == classifier::Kind::Noise;
  if (s.clsPumpKnown) {
    if (other) {
      if (rejected) *rejected = true;
      return false;
    }
    return s.clsKind == classifier::Kind::Pump && s.clsProb >= minProb;
  }
  // Pumpe noch nicht eingelernt: laut + gleichmäßig genügt, außer die KI ist
  // sich sicher, eine Mühle / ein Störgeräusch zu hören.
  if (other && s.clsProb >= minProb) {
    if (rejected) *rejected = true;
    return false;
  }
  return true;
}

void PumpDetector::mergeProfile(PumpProfile& into, const PumpProfile& from) {
  // Hintergrund mitteln; Referenz = leisere der beiden Aufnahmen, Schwankung =
  // tolerantere, damit beide erkannt werden. from ist bereits auf die
  // Verstärkung von into umgerechnet.
  into.bgRmsDb = 0.5f * (into.bgRmsDb + from.bgRmsDb);
  into.pumpRmsDb = fminf(into.pumpRmsDb, from.pumpRmsDb);
  into.steadyThreshold = fmaxf(into.steadyThreshold, from.steadyThreshold);
  into.quality = fminf(into.quality, from.quality);
  ++into.recordings;
}

void PumpDetector::mergeNoise(NoiseProfile::Entry& into, const NoiseProfile::Entry& from) {
  // Pegel der neuen Aufnahme auf die Verstärkung des Eintrags umrechnen;
  // Hintergrund mitteln, Pegel = leiserer (tolerantere Mühlen-Schwelle).
  const float off = gainOffsetDb(into.micGain, from.micGain);
  into.bgRmsDb = 0.5f * (into.bgRmsDb + from.bgRmsDb + off);
  into.levelDb = fminf(into.levelDb, from.levelDb + off);
}

// ---------------------------------------------------------------------------
// Laufende Erkennung
// ---------------------------------------------------------------------------

void PumpDetector::setProfile(const PumpProfile& p) {
  profile_ = p;
  // Standardprofil: immer die aktuellen (strengeren) Grenzen verwenden, auch
  // wenn ein älteres Standardprofil gespeichert wurde.
  if (profile_.isDefault()) profile_.steadyThreshold = cfg::kDefaultSteadyDb;
  autoBgInit_ = false;
  reset();
}

void PumpDetector::updateAutoBackground(float rmsDb) {
  float& bg = profile_.bgRmsDb;
  if (!autoBgInit_) {
    bg = rmsDb;
    autoBgInit_ = true;
  } else if (rmsDb < bg) {
    bg += cfg::kAutoBgFall * (rmsDb - bg);  // leiser geworden: schnell folgen
  } else if (!status_.pumpOn) {
    bg += fminf(rmsDb - bg, cfg::kAutoBgRiseDb);  // lauter: langsam, nie während "Pumpe"
  }
  profile_.pumpRmsDb = bg + cfg::kDefaultSpanDb;
}

void PumpDetector::updateGrinder(uint32_t now, float rawRmsDb) {
  const FrameScores& s = status_.scores;
  const bool on = status_.grinderOn;
  bool raw = false;
  // Die KI sagt "Mühle k" – und sie ist laut genug, gemessen an ihrer eigenen Aufnahme.
  if (s.clsActive && s.clsKind == classifier::Kind::Grinder && grinders_ && s.clsIndex < grinders_->count &&
      !status_.rawActive && !status_.pumpOn) {
    const NoiseProfile::Entry& e = grinders_->entries[s.clsIndex];
    const float lvl = (rawRmsDb + gainOffsetDb(e.micGain, gain_) - e.bgRmsDb) / fmaxf(e.levelDb - e.bgRmsDb, 3.0f);
    raw = lvl >= kGrinderLevel * (on ? kHystLevel : 1.0f) &&
          s.clsProb >= (on ? cfg::kClsHoldProb : cfg::kClsMinProb);
  }

  if (raw) {
    if (grinderSinceMs_ == 0) grinderSinceMs_ = now;
    grinderLastMs_ = now;
    if (!on && now - grinderSinceMs_ >= kGrinderOnMs) {
      status_.grinderOn = true;
      status_.grinderIndex = static_cast<int8_t>(s.clsIndex);
    }
  } else if (grinderSinceMs_ && now - grinderLastMs_ > (on ? kGrinderOffMs : kGrinderGapMs)) {
    grinderSinceMs_ = 0;
    status_.grinderOn = false;
    status_.grinderIndex = -1;
  }
}

void PumpDetector::reset() {
  status_ = DetectorStatus{};
  grinderSinceMs_ = 0;
  steady_.reset();
  loudSinceMs_ = 0;
  activeSinceMs_ = 0;
  onsetMs_ = 0;
  lastActiveMs_ = 0;
}

PumpDetector::Event PumpDetector::process(const AudioFrame& f) {
  // Pegel in den Einheiten des Pumpenprofils (gelernt evtl. mit anderer Verstärkung)
  const float rmsP = f.rmsDb + gainOffsetDb(profile_.micGain, gain_);
  status_.rmsDb = rmsP;
  if (!profile_.valid) {
    status_.scores = FrameScores{};
    status_.rawActive = status_.rejected = status_.grinderOn = false;
    return Event::None;
  }

  // Gleichmäßigkeit über die aktuelle laute Phase
  const bool loud = isLoud(rmsP, profile_, settings_, status_.pumpOn);
  if (loud && steady_.runLength() == 0) loudSinceMs_ = f.timestampMs;
  const float steadiness = steady_.update(rmsP, loud);

  status_.scores = score(f.bandDb, f.rmsDb, steadiness, profile_, gain_, classifier_);
  status_.rawActive = matches(status_.scores, profile_, settings_, status_.pumpOn,
                              &status_.rejected);
  updateGrinder(f.timestampMs, f.rmsDb);
  // Vibration: allein entscheidend bzw. zusätzlich erforderlich
  if (source_ == Source::Vib) {
    status_.rawActive = vibActive_;
    status_.rejected = false;
  } else if (source_ == Source::Both) {
    // Start: Mikrofon UND Vibration (robust gegen Fehlstarts). Läuft die Pumpe,
    // genügt eines davon – unter Druck wird sie oft deutlich leiser, vibriert aber weiter.
    status_.rawActive = status_.pumpOn ? (status_.rawActive || vibActive_) : (status_.rawActive && vibActive_);
  }
  // Nur Mikrofon: Solange die Mühle läuft (inkl. Nachlauf), keine Pumpe
  // starten – das Auslaufen des Mahlwerks klingt kurz pumpenähnlich.
  if (source_ == Source::Mic && status_.grinderOn && !status_.pumpOn && status_.rawActive) {
    status_.rawActive = false;
    status_.rejected = true;
  }

  // Standardprofil: Raumpegel nachführen (nach der Bewertung dieses Frames)
  const bool isDefault = profile_.isDefault();
  if (isDefault) updateAutoBackground(rmsP);
  status_.bgDb = profile_.bgRmsDb;

  // --- Entprellung -------------------------------------------------------------
  const uint32_t now = f.timestampMs;
  const uint32_t onDelay = isDefault && settings_.onDelayMs < cfg::kDefaultMinOnMs
                               ? cfg::kDefaultMinOnMs : settings_.onDelayMs;
  if (!status_.pumpOn) {
    if (status_.rawActive) {
      if (activeSinceMs_ == 0) activeSinceMs_ = now;
      if (now - activeSinceMs_ >= onDelay) {
        status_.pumpOn = true;
        // Timer rückwirkend starten: Gleichmäßigkeit und KI brauchen
        // nach dem Einschalten einige Frames, bis sie sicher passen. Der
        // tatsächliche Beginn ist der Anfang der (ununterbrochen) lauten Phase –
        // höchstens kMaxBackdateMs zurück, damit ein vorheriges anderes
        // Geräusch nicht mitgezählt wird.
        const uint32_t earliest = activeSinceMs_ - kMaxBackdateMs;
        const uint32_t loudSince = source_ == Source::Vib ? vibSinceMs_ : loudSinceMs_;
        onsetMs_ = static_cast<int32_t>(earliest - loudSince) > 0 ? earliest : loudSince;
        lastActiveMs_ = now;
        return Event::Started;
      }
    } else {
      activeSinceMs_ = 0;
    }
  } else {
    // Sicherung: unplausibel lange "Pumpe" -> beenden. Beim Standardprofil
    // ist es dann wohl ein neues Dauergeräusch: als Hintergrund übernehmen.
    const uint32_t maxMs = isDefault ? cfg::kMaxPumpMsDefault : cfg::kMaxPumpMsLearned;
    const bool tooLong = now - onsetMs_ > maxMs;
    if (tooLong && isDefault) {
      profile_.bgRmsDb = rmsP;
      profile_.pumpRmsDb = rmsP + cfg::kDefaultSpanDb;
    }
    if (status_.rawActive && !tooLong) {
      lastActiveMs_ = now;
    } else if (tooLong || now - lastActiveMs_ >= settings_.offDelayMs) {
      status_.pumpOn = false;
      activeSinceMs_ = 0;
      return Event::Stopped;  // Ende = lastActiveMs_ (letzter aktiver Frame)
    }
  }
  return Event::None;
}
