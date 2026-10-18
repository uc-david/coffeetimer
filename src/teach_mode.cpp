// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file teach_mode.cpp
 * @brief Implementierung des Teach-Modus und der Qualitätsbewertung.
 */
#include "teach_mode.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdarg.h>

#include <algorithm>
#include <vector>

#include "pump_detector.h"

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
namespace {

/// Großen Puffer bevorzugt im PSRAM anlegen.
void* allocLarge(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(bytes);
}

/// Perzentil (q = 0..1) einer Werteliste. Verändert die Reihenfolge.
float percentile(std::vector<float>& v, float q) {
  if (v.empty()) return 0.0f;
  const size_t idx = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5f));
  std::nth_element(v.begin(), v.begin() + idx, v.end());
  return v[idx];
}

/// Mittelwert und Standardabweichung der Pegel einer Frame-Liste.
void rmsStats(const AudioFrame* frames, size_t n, float& mean, float& stdDev) {
  if (n == 0) { mean = -90.0f; stdDev = 0.0f; return; }
  double sum = 0.0, sq = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sum += frames[i].rmsDb;
    sq += frames[i].rmsDb * frames[i].rmsDb;
  }
  mean = static_cast<float>(sum / n);
  stdDev = sqrtf(fmaxf(static_cast<float>(sq / n) - mean * mean, 0.0f));
}

/// Mittlerer Übersteuerungsanteil einer Frame-Liste.
float meanClip(const AudioFrame* frames, size_t n) {
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) sum += frames[i].clipRatio;
  return n ? static_cast<float>(sum / n) : 0.0f;
}

/// Lineare Abbildung von x aus [a,b] auf [0,1], begrenzt.
float ramp(float x, float a, float b) {
  return constrain((x - a) / (b - a), 0.0f, 1.0f);
}

}  // namespace

// ---------------------------------------------------------------------------
// TeachResult
// ---------------------------------------------------------------------------
void TeachResult::addMessage(const char* fmt, ...) {
  if (messageCount >= kMaxMessages) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(messages[messageCount], kMessageLen, fmt, args);
  va_end(args);
  ++messageCount;
}

// ---------------------------------------------------------------------------
// TeachController
// ---------------------------------------------------------------------------
void TeachController::begin() {
  // Pumpe: bis 60 s ≈ 1900 Frames -> ~330 kB Merkmale.
  bgFrames_ = static_cast<AudioFrame*>(allocLarge(sizeof(AudioFrame) * cfg::kTeachMaxBgFrames));
  pumpFrames_ = static_cast<AudioFrame*>(allocLarge(sizeof(AudioFrame) * cfg::kTeachMaxPumpFrames));
}

void TeachController::start(uint32_t nowMs, TeachMode mode, const DetectorSettings& current,
                            const PumpProfile& profile, const GrinderProfile* grinders, uint8_t noiseCount,
                            const classifier::Classifier* cls, float gain) {
  mode_ = mode;
  grinders_ = grinders;
  noiseCount_ = noiseCount;
  classifier_ = cls;
  gain_ = gain;
  clearResult();
  settings_ = current;
  currentProfile_ = profile;
  pumpCount_ = 0;
  triggerFrames_ = 0;
  recordedMs_ = 0;
  finishRequested_ = false;

  // Jedes Geräusch nimmt seine eigene Ruhe auf -> keine Abhängigkeit vom
  // Hintergrund anderer Aufnahmen oder des Pumpenprofils.
  bgCount_ = 0;
  bgRmsDb_ = -90.0f;
  enterPhase(TeachPhase::Background, nowMs);
  if (!bgFrames_ || !pumpFrames_) {
    fail("Nicht genug Speicher für die Aufnahme (PSRAM).");
  }
}

void TeachController::clearResult() {
  resetInPlace(result_);  // ~1,4 kB – kein Temporärobjekt auf dem Stack
  result_.mode = mode_;
}

void TeachController::cancel() {
  phase_ = TeachPhase::Idle;
  progress_ = 0.0f;
}

void TeachController::enterPhase(TeachPhase p, uint32_t nowMs) {
  phase_ = p;
  phaseStartMs_ = nowMs;
  progress_ = 0.0f;
}

const char* TeachController::modeName(TeachMode m) {
  switch (m) {
    case TeachMode::Pump:       return "pump";
    case TeachMode::Noise:      return "noise";
    case TeachMode::Grinder:    return "grinder";
  }
  return "pump";
}

TeachMode TeachController::modeFromName(const char* name) {
  for (TeachMode m : {TeachMode::Noise, TeachMode::Grinder}) {
    if (strcmp(name, modeName(m)) == 0) return m;
  }
  return TeachMode::Pump;
}

const char* TeachController::phaseName(TeachPhase p) {
  switch (p) {
    case TeachPhase::Idle:       return "idle";
    case TeachPhase::Background: return "background";
    case TeachPhase::WaitPump:   return "waitPump";
    case TeachPhase::RecordPump: return "recordPump";
    case TeachPhase::Done:       return "done";
  }
  return "?";
}

const char* TeachController::instruction() const {
  const bool error = result_.quality == TeachQuality::Error;
  if (isNoiseMode(mode_)) {
    const bool grinder = mode_ == TeachMode::Grinder;
    switch (phase_) {
      case TeachPhase::Background:
        return grinder ? "Schritt 1/3: Ruhe bitte! Mühle und Maschine AUS – Hintergrund wird aufgenommen."
                       : "Schritt 1/3: Ruhe bitte! Gerät noch AUS – Hintergrund wird aufgenommen.";
      case TeachPhase::WaitPump:
        return grinder ? "Schritt 2/3: Jetzt die MÜHLE mahlen lassen."
                       : "Schritt 2/3: Jetzt das Störgeräusch einschalten (z. B. Milchaufschäumer).";
      case TeachPhase::RecordPump:
        return grinder ? "Schritt 3/3: Aufnahme läuft – normal mahlen, danach ausschalten."
                       : "Schritt 3/3: Aufnahme läuft – Gerät normal laufen lassen, danach ausschalten.";
      case TeachPhase::Done:
        return error ? "Aufnahme nicht verwendbar – bitte wiederholen."
                     : grinder ? "Aufnahme fertig – als Mühle speichern oder ergänzen?"
                               : "Aufnahme fertig – Störgeräusch speichern oder ergänzen?";
      default:
        break;
    }
  }
  switch (phase_) {
    case TeachPhase::Idle:
      return "Teach-Modus nicht aktiv.";
    case TeachPhase::Background:
      return "Schritt 1/3: Ruhe bitte! Pumpe AUS lassen – Hintergrund wird aufgenommen.";
    case TeachPhase::WaitPump:
      return "Schritt 2/3: Jetzt einen normalen BEZUG starten (am besten mit Kaffee).";
    case TeachPhase::RecordPump:
      return "Schritt 3/3: Aufnahme läuft – Bezug wie gewohnt beenden. "
             "Endet automatisch, wenn die Pumpe aus ist.";
    case TeachPhase::Done:
      return error ? "Aufnahme nicht verwendbar – bitte wiederholen."
                   : "Aufnahme fertig – Profil ersetzen oder ergänzen.";
  }
  return "";
}

void TeachController::process(const AudioFrame& f) {
  if (!recording()) return;

  liveRmsDb_ = f.rmsDb;
  const uint32_t elapsed = f.timestampMs - phaseStartMs_;

  switch (phase_) {
    case TeachPhase::Background: {
      // Erste Millisekunden verwerfen: Geräusch der Touch-Berührung o. ä.
      if (elapsed >= cfg::kTeachBackgroundSkipMs && bgCount_ < cfg::kTeachMaxBgFrames) {
        bgFrames_[bgCount_++] = f;
      }
      progress_ = constrain(static_cast<float>(elapsed) / cfg::kTeachBackgroundMs, 0.0f, 1.0f);
      if (elapsed >= cfg::kTeachBackgroundMs) {
        finishBackground();
        if (phase_ == TeachPhase::Background) enterPhase(TeachPhase::WaitPump, f.timestampMs);
      }
      break;
    }

    case TeachPhase::WaitPump: {
      // Pumpe gilt als eingeschaltet, wenn der Pegel einige Frames in Folge
      // deutlich über dem Hintergrund liegt.
      if (f.rmsDb >= bgRmsDb_ + cfg::kTeachPumpTriggerDb) {
        if (++triggerFrames_ >= cfg::kTeachPumpTriggerFrames) {
          enterPhase(TeachPhase::RecordPump, f.timestampMs);
          lastLoudMs_ = f.timestampMs;
          lastLoudIndex_ = 0;
          break;
        }
      } else {
        triggerFrames_ = 0;
      }
      progress_ = constrain(static_cast<float>(elapsed) / cfg::kTeachWaitPumpTimeoutMs, 0.0f, 1.0f);
      if (elapsed >= cfg::kTeachWaitPumpTimeoutMs) {
        fail("Nichts gehört: Der Pegel stieg in %u s nicht um mindestens %.0f dB über "
             "den Hintergrund (%.1f dBFS).",
             (unsigned)(cfg::kTeachWaitPumpTimeoutMs / 1000), cfg::kTeachPumpTriggerDb, bgRmsDb_);
        result_.addMessage(!isNoiseMode(mode_)
            ? "Tipp: Gerät näher an die Maschine stellen (Pumpe sitzt meist unten/hinten) und "
              "die Pumpe während Schritt 2 einschalten."
            : "Tipp: Das Geräusch während Schritt 2 einschalten. Ist es sehr leise, stört "
              "es die Erkennung ohnehin nicht.");
      }
      break;
    }

    case TeachPhase::RecordPump: {
      recordedMs_ = elapsed;
      if (elapsed >= cfg::kTeachPumpSkipMs && pumpCount_ < cfg::kTeachMaxPumpFrames) {
        pumpFrames_[pumpCount_++] = f;
      }
      // Ende erkennen: Pumpe lange genug still (Pausen der Pre-Infusion
      // werden überbrückt), Puffer voll oder Maximaldauer erreicht.
      if (f.rmsDb >= bgRmsDb_ + cfg::kTeachActiveDb) {
        lastLoudMs_ = f.timestampMs;
        lastLoudIndex_ = pumpCount_;
      }
      // Störgeräusche (Mühle) haben keine Pre-Infusion -> kürzere Stille genügt.
      const bool noise = isNoiseMode(mode_);
      const uint32_t silenceMs = noise ? cfg::kTeachNoiseStopSilenceMs : cfg::kTeachStopSilenceMs;
      const uint32_t maxMs = noise ? cfg::kTeachNoiseMaxMs : cfg::kTeachPumpMaxMs;
      progress_ = constrain(static_cast<float>(elapsed) / maxMs, 0.0f, 1.0f);
      if (finishRequested_ || f.timestampMs - lastLoudMs_ >= silenceMs || elapsed >= maxMs ||
          pumpCount_ >= cfg::kTeachMaxPumpFrames) {
        if (noise) {
          analyzeNoise();
        } else {
          analyzePump();
        }
      }
      break;
    }

    default:
      break;
  }
}

void TeachController::trainingFrames(bool background, std::vector<int8_t>& out, size_t maxFrames) const {
  out.clear();
  const AudioFrame* frames = background ? bgFrames_ : pumpFrames_;
  const size_t n = background ? bgCount_ : pumpCount_;
  if (!frames || !n || !maxFrames) return;
  // Hörbare Frames der Aufnahme (wie in der Auswertung); Ruhe: alle
  std::vector<size_t> use;
  use.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (background || frames[i].rmsDb >= bgRmsDb_ + cfg::kTeachActiveDb) use.push_back(i);
  }
  const size_t take = use.size() < maxFrames ? use.size() : maxFrames;
  out.resize(take * cfg::kNumBands);
  for (size_t k = 0; k < take; ++k) {
    const size_t i = use[k * use.size() / take];  // gleichmäßig verteilt
    classifier::quantizeShape(frames[i].bandDb, &out[k * cfg::kNumBands]);
  }
}

void TeachController::fail(const char* fmt, ...) {
  clearResult();
  result_.quality = TeachQuality::Error;
  result_.score = 0;
  va_list args;
  va_start(args, fmt);
  vsnprintf(result_.messages[0], TeachResult::kMessageLen, fmt, args);
  va_end(args);
  result_.messageCount = 1;
  phase_ = TeachPhase::Done;
  progress_ = 1.0f;
}

void TeachController::finishBackground() {
  float stdDev;
  rmsStats(bgFrames_, bgCount_, bgRmsDb_, stdDev);

  // Bereits hier abbrechen, wenn der Hintergrund unbrauchbar ist – sonst würde
  // der Benutzer unnötig auf die Pumpe warten.
  if (bgCount_ < 10) {
    fail("Hintergrundaufnahme fehlgeschlagen (zu wenige Audiodaten). Mikrofon prüfen.");
    return;
  }
  const float clip = meanClip(bgFrames_, bgCount_);
  if (clip > cfg::kQualMaxClipRatio) {
    fail("Hintergrund übersteuert (%.1f %% der Samples) – lautes Störgeräusch während "
         "Schritt 1. Alles aus? Bitte in Ruhe wiederholen.", clip * 100.0f);
  }
}

void TeachController::classifyRecording(const std::vector<size_t>& active) {
  TeachResult& r = result_;
  const classifier::Classifier* cls = classifier_;
  r.clsReady = cls && cls->ready() && !active.empty();
  if (!r.clsReady) return;

  // Welche Mühle / welches Störgeräusch hört die KI am häufigsten?
  size_t grinderHits[cfg::kMaxGrinders] = {}, noiseHits[cfg::kMaxNoises] = {};
  for (size_t i : active) {
    const classifier::Result c = cls->classify(pumpFrames_[i].bandDb);
    if (c.kind == classifier::Kind::Grinder && c.index < cfg::kMaxGrinders) ++grinderHits[c.index];
    if (c.kind == classifier::Kind::Noise && c.index < cfg::kMaxNoises) ++noiseHits[c.index];
  }
  auto best = [&](const size_t* hits, size_t count, int8_t& match, float& share) {
    for (size_t k = 0; k < count; ++k) {
      const float sh = static_cast<float>(hits[k]) / active.size();
      if (sh > share) {
        share = sh;
        match = static_cast<int8_t>(k);
      }
    }
    // Eindeutig nur ab 20 % der Klangbilder; mit genau einem Eintrag ist es dieser.
    if (share < 0.2f) match = count == 1 ? 0 : -1;
  };
  best(grinderHits, grinders_ ? grinders_->count : 0, r.grinderMatch, r.grinderMatchShare);
  best(noiseHits, noiseCount_, r.noiseMatch, r.noiseMatchShare);
}

namespace {
/// Anteil der Frames, die mit Profil p wie im Betrieb als Pumpe erkannt würden
/// (Pegel, Gleichmäßigkeit in zeitlicher Reihenfolge, KI).
float shareAsPump(const AudioFrame* frames, size_t n, const std::vector<size_t>& active, const PumpProfile& p,
                  const DetectorSettings& st, float gain, const classifier::Classifier* cls) {
  if (active.empty()) return 0.0f;
  const float off = PumpDetector::gainOffsetDb(p.micGain, gain);
  std::vector<float> steady(n);
  SteadinessTracker tr;
  for (size_t i = 0; i < n; ++i) {
    steady[i] = tr.update(frames[i].rmsDb + off, PumpDetector::isLoud(frames[i].rmsDb + off, p, st, false));
  }
  size_t hits = 0;
  for (size_t i : active) {
    const FrameScores s = PumpDetector::score(frames[i].bandDb, frames[i].rmsDb, steady[i], p, gain, cls);
    hits += PumpDetector::matches(s, p, st, false);
  }
  return static_cast<float>(hits) / active.size();
}
}  // namespace

void TeachController::analyzePump() {
  TeachResult& r = result_;
  clearResult();
  PumpProfile& p = r.profile;
  p.micGain = gain_;  // Frames kommen unverändert mit der aktuellen Verstärkung

  // Stille am Ende (Pumpe bereits aus) abschneiden.
  pumpCount_ = std::min(pumpCount_, lastLoudIndex_);

  // --- Hintergrund ------------------------------------------------------------
  rmsStats(bgFrames_, bgCount_, p.bgRmsDb, r.bgStdDb);

  // --- Hörbare Pumpen-Frames bestimmen, Pausen messen -------------------------
  const float activeLimit = p.bgRmsDb + cfg::kTeachActiveDb;
  std::vector<size_t> active;
  active.reserve(pumpCount_);
  size_t gap = 0, longestGap = 0;
  for (size_t i = 0; i < pumpCount_; ++i) {
    if (pumpFrames_[i].rmsDb >= activeLimit) {
      if (!active.empty()) longestGap = std::max(longestGap, gap);
      gap = 0;
      active.push_back(i);
    } else {
      ++gap;
    }
  }
  r.activeSeconds = active.size() * cfg::kFrameSize / static_cast<float>(cfg::kSampleRate);
  r.longestPauseMs = static_cast<uint32_t>(longestGap * cfg::kFrameSize * 1000 / cfg::kSampleRate);
  r.clipRatio = meanClip(pumpFrames_, pumpCount_);
  r.trainFrames = static_cast<uint16_t>(std::min(active.size(), classifier::kMaxFramesPerRecord));

  if (active.size() * cfg::kFrameSize * 1000 / cfg::kSampleRate < cfg::kTeachPumpMinActiveMs) {
    fail("Pumpe nur %.1f s hörbar (mind. %.0f s nötig). Bezug länger laufen lassen bzw. "
         "Gerät näher an die Pumpe stellen.",
         r.activeSeconds, cfg::kTeachPumpMinActiveMs / 1000.0f);
    return;
  }

  // --- Pegel: Referenz = leiser Teil der Pumpe ---------------------------------
  {
    std::vector<float> levels;
    levels.reserve(active.size());
    for (size_t i : active) levels.push_back(pumpFrames_[i].rmsDb);
    p.pumpRmsDb = percentile(levels, 0.10f);
  }
  r.snrDb = p.pumpRmsDb - p.bgRmsDb;
  p.valid = true;
  p.recordings = 1;

  // --- Gleichmäßigkeit je Frame (in zeitlicher Reihenfolge, wie im Betrieb) ----
  {
    SteadinessTracker tr;
    std::vector<float> valid;
    for (size_t i = 0, a = 0; i < pumpCount_; ++i) {
      const float st = tr.update(pumpFrames_[i].rmsDb, PumpDetector::isLoud(pumpFrames_[i].rmsDb, p, settings_, false));
      const bool isActive = a < active.size() && active[a] == i;
      if (isActive) ++a;
      if (isActive && st < SteadinessTracker::kUnsteady) valid.push_back(st);
    }
    // Schwelle: deutlich über der Schwankung der Pumpe (95 %), mind. 1,5 dB.
    r.pumpSteadiness = valid.size() >= 10 ? percentile(valid, 0.95f) : 4.0f;
    p.steadyThreshold = constrain(r.pumpSteadiness * 1.5f + 0.5f, 1.5f, 6.0f);
  }

  // Ausschaltverzögerung muss die längste Pause (Pre-Infusion) überbrücken.
  r.suggestedOffDelayMs = settings_.offDelayMs;
  if (r.longestPauseMs + 400 > settings_.offDelayMs) {
    r.suggestedOffDelayMs = static_cast<uint16_t>(std::min<uint32_t>(r.longestPauseMs + 400, 5000));
  }

  // --- Was hört die bisherige KI? (mit den neuen Pegeln) ------------------------
  classifyRecording(active);
  const bool pumpKnown = r.clsReady && classifier_->hasKind(classifier::Kind::Pump);
  if (pumpKnown) r.clsAsPump = shareAsPump(pumpFrames_, pumpCount_, active, p, settings_, gain_, classifier_);

  // --- Bewertung ------------------------------------------------------------------
  bool error = false, warning = false;
  if (r.clipRatio > cfg::kQualMaxClipRatio) {
    error = true;
    r.addMessage("Übersteuert: %.1f %% der Samples am Anschlag. Gerät weiter von der "
                 "Pumpe entfernt aufstellen bzw. Mikrofon-Verstärkung verringern.", r.clipRatio * 100.0f);
  }
  if (r.snrDb < cfg::kQualMinSnrDb) {
    error = true;
    r.addMessage("Pumpe im leisen Teil nur %.1f dB über dem Hintergrund (mind. %.0f dB). "
                 "Näher an die Pumpe oder Störquellen (Radio, Mühle, Gespräche) abstellen.",
                 r.snrDb, cfg::kQualMinSnrDb);
  }
  if (!error && r.snrDb < cfg::kQualGoodSnrDb) {
    warning = true;
    r.addMessage("Geringer Pegelabstand im leisen Teil (%.1f dB). Erkennung kann bei "
                 "Nebengeräuschen unzuverlässig sein.", r.snrDb);
  }
  if (r.bgStdDb > cfg::kQualBgStdWarnDb) {
    warning = true;
    r.addMessage("Hintergrund war unruhig (±%.1f dB). Teach am besten in ruhiger Umgebung "
                 "wiederholen.", r.bgStdDb);
  }
  if (!error) {
    r.addMessage("Pumpe läuft gleichmäßig (±%.1f dB) – unruhigere Geräusche über ±%.1f dB "
                 "(Niesen, Sprechen, Klappern) werden ignoriert.", r.pumpSteadiness, p.steadyThreshold);
    if (pumpKnown) {
      r.addMessage("Die bisherige KI erkennt %.0f %% dieser Aufnahme als Pumpe%s", r.clsAsPump * 100.0f,
                   r.clsAsPump < 0.8f ? " – „Ergänzen“ bringt ihr diese Variante bei." : ".");
    }
    r.addMessage("%u Klangbilder aus %.1f s Pumpenlauf für die KI.", (unsigned)r.trainFrames, r.activeSeconds);
  }
  if (r.suggestedOffDelayMs > settings_.offDelayMs) {
    r.addMessage("Pause von %.1f s erkannt (Pre-Infusion?) – Ausschaltverzögerung wird auf "
                 "%.1f s gesetzt.", r.longestPauseMs / 1000.0f, r.suggestedOffDelayMs / 1000.0f);
  }

  // --- Gesamtnote 0..100 ---
  float score = 50.0f * ramp(r.snrDb, cfg::kQualMinSnrDb, 20.0f) +
                25.0f * (1.0f - ramp(r.bgStdDb, 1.0f, 6.0f)) +
                15.0f * ramp(r.activeSeconds, 3.0f, 15.0f) +
                10.0f * (r.clipRatio <= cfg::kQualMaxClipRatio ? 1.0f : 0.0f);
  if (error) score = fminf(score, 30.0f);
  r.score = static_cast<int>(lroundf(score));
  p.quality = static_cast<float>(r.score);

  r.quality = error ? TeachQuality::Error : (warning ? TeachQuality::Warning : TeachQuality::Ok);
  if (r.quality == TeachQuality::Ok) {
    // Positive Zusammenfassung an den Anfang stellen.
    char summary[TeachResult::kMessageLen];
    snprintf(summary, sizeof(summary), "Aufnahme gut: Pumpe %.1f dB über Hintergrund, %.1f s hörbar.", r.snrDb,
             r.activeSeconds);
    if (r.messageCount < TeachResult::kMaxMessages) {
      memmove(r.messages[1], r.messages[0], TeachResult::kMessageLen * r.messageCount);
      ++r.messageCount;
    }
    strncpy(r.messages[0], summary, TeachResult::kMessageLen);
  }

  Serial.printf("[teach] snr=%.1f bgStd=%.1f aktiv=%.1fs pause=%lums steady(p95)=%.2f steadyTh=%.2f "
                "kiPumpe=%.2f klangbilder=%u score=%d\n",
                r.snrDb, r.bgStdDb, r.activeSeconds, (unsigned long)r.longestPauseMs, r.pumpSteadiness,
                p.steadyThreshold, r.clsAsPump, (unsigned)r.trainFrames, r.score);

  phase_ = TeachPhase::Done;
  progress_ = 1.0f;
}

void TeachController::analyzeNoise() {
  TeachResult& r = result_;
  clearResult();
  NoiseProfile::Entry& e = r.noise;
  const bool grinder = mode_ == TeachMode::Grinder;
  const char* what = grinder ? "Mühle" : "Störgeräusch";

  // Eigener Hintergrund (Schritt 1) und eigene Verstärkung des Geräuschs.
  e.micGain = gain_;
  rmsStats(bgFrames_, bgCount_, e.bgRmsDb, r.bgStdDb);

  // Stille am Ende abschneiden, hörbare Frames bestimmen.
  pumpCount_ = std::min(pumpCount_, lastLoudIndex_);
  const float activeLimit = e.bgRmsDb + cfg::kTeachActiveDb;
  std::vector<size_t> active;
  for (size_t i = 0; i < pumpCount_; ++i) {
    if (pumpFrames_[i].rmsDb >= activeLimit) active.push_back(i);
  }
  r.activeSeconds = active.size() * cfg::kFrameSize / static_cast<float>(cfg::kSampleRate);
  r.clipRatio = meanClip(pumpFrames_, pumpCount_);
  r.trainFrames = static_cast<uint16_t>(std::min(active.size(), classifier::kMaxFramesPerRecord));
  if (r.activeSeconds * 1000.0f < cfg::kTeachNoiseMinActiveMs) {
    fail("%s nur %.1f s hörbar (mind. %.0f s nötig). Länger laufen lassen.", what,
         r.activeSeconds, cfg::kTeachNoiseMinActiveMs / 1000.0f);
    return;
  }

  // Eigener Pegel: leiser Teil des Geräuschs (10 %-Perzentil)
  {
    std::vector<float> levels;
    levels.reserve(active.size());
    for (size_t i : active) levels.push_back(pumpFrames_[i].rmsDb);
    e.levelDb = percentile(levels, 0.10f);
  }
  r.snrDb = e.levelDb - e.bgRmsDb;

  // --- Was hört die bisherige KI? ----------------------------------------------
  classifyRecording(active);
  const bool pumpKnown = r.clsReady && classifier_->hasKind(classifier::Kind::Pump);
  // Wie oft würde es bisher als Pumpe erkannt (Pegel des Pumpenprofils, KI)?
  r.clsAsPump = shareAsPump(pumpFrames_, pumpCount_, active, currentProfile_, settings_, gain_,
                            r.clsReady ? classifier_ : nullptr);

  // --- Bewertung -------------------------------------------------------------
  bool error = false, warning = false;
  if (r.clipRatio > cfg::kQualMaxClipRatio) {
    error = true;
    r.addMessage("Übersteuert (%.1f %% der Samples) – Gerät weiter entfernt aufnehmen.",
                 r.clipRatio * 100.0f);
  }
  if (r.bgStdDb > cfg::kQualBgStdWarnDb) {
    warning = true;
    r.addMessage("Hintergrund war unruhig (±%.1f dB) – Schritt 1 am besten in Ruhe wiederholen.", r.bgStdDb);
  }
  if (!error) {
    r.addMessage("Bisher zu %.0f %% als Pumpe erkannt%s", r.clsAsPump * 100.0f,
                 !pumpKnown ? " (Pumpe noch nicht eingelernt)." : ".");
    if (!grinder && r.clsAsPump < 0.02f) {
      r.addMessage("Wurde ohnehin nicht als Pumpe erkannt – Speichern schadet nicht und schärft die KI.");
    }
  }
  // Ergänzen-Vorschlag: was die KI in der Aufnahme hört
  if (grinder && grinders_ && r.grinderMatch >= 0) {
    if (r.grinderMatchShare >= 0.2f) {
      r.addMessage("Die KI hört zu %.0f %% „%s“ – „Ergänzen“, wenn es dieselbe Mühle ist, sonst als neue "
                   "Mühle speichern.", r.grinderMatchShare * 100.0f, grinders_->names[r.grinderMatch]);
    } else {
      r.addMessage("„Ergänzen“ fügt die Aufnahme „%s“ hinzu – bei einer anderen Mühle als neue Mühle speichern.",
                   grinders_->names[r.grinderMatch]);
    }
  }
  if (!grinder && r.noiseMatch >= 0) {
    if (r.noiseMatchShare >= 0.2f) {
      r.addMessage("Die KI hört zu %.0f %% Störgeräusch %d – „Ergänzen“, wenn es dasselbe Gerät ist.",
                   r.noiseMatchShare * 100.0f, r.noiseMatch + 1);
    } else {
      r.addMessage("„Ergänzen“ fügt die Aufnahme Störgeräusch %d hinzu.", r.noiseMatch + 1);
    }
  }
  // Liste voll: Beim Speichern als neuer Eintrag fällt das älteste weg – vorher sagen.
  if (!grinder && noiseCount_ >= cfg::kMaxNoises) {
    warning = true;
    r.addMessage("Liste voll (%u Störgeräusche): Als neues Störgeräusch gespeichert, wird Störgeräusch 1 (das "
                 "älteste) gelöscht – „Ergänzen“ vermeidet das.", (unsigned)cfg::kMaxNoises);
  }
  if (grinder && grinders_ && grinders_->count >= cfg::kMaxGrinders) {
    r.addMessage("Mühlen-Liste voll (%u): Die Aufnahme kann nur einer vorhandenen Mühle hinzugefügt werden.",
                 (unsigned)cfg::kMaxGrinders);
  }
  r.addMessage("%.1f s aufgenommen (%.1f dB über dem Hintergrund), %u Klangbilder für die KI.", r.activeSeconds,
               r.snrDb, (unsigned)r.trainFrames);

  float score = 60.0f * ramp(r.snrDb, cfg::kTeachPumpTriggerDb, 20.0f) +
                20.0f * (1.0f - ramp(r.bgStdDb, 1.0f, 6.0f)) +
                20.0f * ramp(r.activeSeconds, 2.0f, 10.0f);
  if (error) score = fminf(score, 30.0f);
  r.score = constrain(static_cast<int>(lroundf(score)), 0, 100);
  r.quality = error ? TeachQuality::Error : (warning ? TeachQuality::Warning : TeachQuality::Ok);

  Serial.printf("[teach-noise] aktiv=%.1fs snr=%.1f alsPumpe=%.2f muehle=%d(%.2f) stoer=%d(%.2f)\n",
                r.activeSeconds, r.snrDb, r.clsAsPump, r.grinderMatch, r.grinderMatchShare, r.noiseMatch,
                r.noiseMatchShare);

  phase_ = TeachPhase::Done;
  progress_ = 1.0f;
}
