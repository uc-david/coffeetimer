/**
 * @file pump_detector.cpp
 * @brief Implementierung der Pumpenerkennung.
 */
#include "pump_detector.h"

#include <math.h>
#include <string.h>

namespace {

/// Faktoren der Hysterese: Ist die Pumpe bereits EIN, genügen etwas
/// niedrigere Werte, um EIN zu bleiben. Verhindert Flattern an der Schwelle.
constexpr float kHystSimilarity = 0.90f;
constexpr float kHystPeriodicity = 0.85f;
constexpr float kHystRhythm = 0.85f;
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

/// Skalarprodukt zweier Bandvektoren.
float dotBands(const float* a, const float* b) {
  float s = 0.0f;
  for (size_t i = 0; i < cfg::kNumBands; ++i) s += a[i] * b[i];
  return s;
}

/**
 * Gesamtwert "klingt wie ...": bei Pumpen mit Takt zählen Rhythmus-Form und
 * Klangmuster gemeinsam, sonst nur das Klangmuster. Dient dem Vergleich
 * Pumpe gegen Störgeräusch.
 */
float combined(bool periodic, float similarity, float rhythm) {
  return periodic ? 0.5f * (similarity + rhythm) : similarity;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gemeinsame Berechnungen
// ---------------------------------------------------------------------------

float PumpDetector::excessVector(const float* bandDb, const float* bgBandDb, float* out) {
  // 1. Nur der Anteil über dem Hintergrund zählt. Dadurch spielt konstantes
  //    Grundrauschen keine Rolle – verglichen wird, was die Quelle "hinzufügt".
  float level = 0.0f, mean = 0.0f;
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    out[b] = fmaxf(bandDb[b] - bgBandDb[b], 0.0f);
    level += out[b] * out[b];
    mean += out[b];
  }
  level = sqrtf(level);
  mean /= cfg::kNumBands;
  // 2. Mittelwert abziehen: übrig bleibt die Klangfarbe (welche Bänder relativ
  //    stärker/schwächer sind), unabhängig davon, wie laut es insgesamt ist.
  float norm = 0.0f;
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    out[b] -= mean;
    norm += out[b] * out[b];
  }
  norm = sqrtf(norm);
  if (level < 1e-3f || norm < 1e-3f) {
    for (size_t b = 0; b < cfg::kNumBands; ++b) out[b] = 0.0f;
    return 0.0f;
  }
  for (size_t b = 0; b < cfg::kNumBands; ++b) out[b] /= norm;
  return level;
}

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

float PumpDetector::bestTemplate(const float* excess, const float (*templates)[cfg::kNumBands],
                                 size_t count) {
  float best = 0.0f;
  for (size_t t = 0; t < count; ++t) best = fmaxf(best, dotBands(excess, templates[t]));
  return best;
}

float PumpDetector::levelRatio(float rmsDb, const PumpProfile& p) {
  const float span = fmaxf(p.pumpRmsDb - p.bgRmsDb, 1.0f);
  return (rmsDb - p.bgRmsDb) / span;
}

void PumpDetector::normalizeCurve(const float* in, float* out) {
  float mean = 0.0f;
  for (size_t k = 0; k < cfg::kAcfLen; ++k) mean += in[k];
  mean /= cfg::kAcfLen;
  float norm = 0.0f;
  for (size_t k = 0; k < cfg::kAcfLen; ++k) {
    out[k] = in[k] - mean;
    norm += out[k] * out[k];
  }
  norm = sqrtf(norm);
  for (size_t k = 0; k < cfg::kAcfLen; ++k) out[k] = norm > 1e-6f ? out[k] / norm : 0.0f;
}

float PumpDetector::rhythmSimilarity(const float* curve, const float* normalizedTemplate) {
  // Pearson-Korrelation: vergleicht nur die Form der Kurve (wo Spitzen und
  // Täler liegen), nicht ihre Höhe.
  float mean = 0.0f;
  for (size_t k = 0; k < cfg::kAcfLen; ++k) mean += curve[k];
  mean /= cfg::kAcfLen;
  float dot = 0.0f, norm = 0.0f;
  for (size_t k = 0; k < cfg::kAcfLen; ++k) {
    const float v = curve[k] - mean;
    dot += v * normalizedTemplate[k];
    norm += v * v;
  }
  return norm > 1e-9f ? dot / sqrtf(norm) : 0.0f;
}

float PumpDetector::periodicityAt(const float* curve, float lag) {
  if (lag <= 0.0f) return 0.0f;
  int lo = static_cast<int>(floorf(lag * (1.0f - cfg::kLagTolerance)));
  int hi = static_cast<int>(ceilf(lag * (1.0f + cfg::kLagTolerance)));
  if (hi - lo < 2) { --lo; ++hi; }  // mindestens ±1 Sample Toleranz
  lo = lo < (int)cfg::kMinLag ? (int)cfg::kMinLag : lo;
  hi = hi > (int)cfg::kMaxLag ? (int)cfg::kMaxLag : hi;
  float best = -1.0f;
  for (int k = lo; k <= hi; ++k) best = fmaxf(best, curve[k - cfg::kMinLag]);
  return best;
}

bool PumpDetector::isLoud(float rmsDb, const PumpProfile& p, const DetectorSettings& st,
                          bool hysteresis) {
  return levelRatio(rmsDb, p) >= st.levelThreshold * (hysteresis ? kHystLevel : 1.0f);
}

float PumpDetector::gainOffsetDb(float learnedGain, float currentGain) {
  return learnedGain > 0.0f && currentGain > 0.0f ? 20.0f * log10f(learnedGain / currentGain) : 0.0f;
}

FrameScores PumpDetector::score(const float* bandDb, float rmsDb, const float* curve,
                                float steadiness, const PumpProfile& p, const NoiseProfile& n,
                                const GrinderProfile* g, float gain) {
  FrameScores s;
  s.steadiness = steadiness;
  // Jedes Geräusch wird mit seiner eigenen Verstärkung und seinem eigenen
  // Hintergrund verglichen (keine Abhängigkeit zwischen den Geräuschen).
  float shifted[cfg::kNumBands];
  auto excessFor = [&](float learnedGain, const float* bg, float* out) {
    const float off = gainOffsetDb(learnedGain, gain);
    for (size_t b = 0; b < cfg::kNumBands; ++b) shifted[b] = bandDb[b] + off;
    return excessVector(shifted, bg, out) > 0.0f;
  };
  float x[cfg::kNumBands];
  const bool audible = excessFor(p.micGain, p.bgBandDb, x);

  s.similarity = audible ? bestTemplate(x, p.templates, p.templateCount) : 0.0f;
  s.levelRatio = levelRatio(rmsDb + gainOffsetDb(p.micGain, gain), p);
  if (p.periodic) {
    s.periodicity = periodicityAt(curve, p.periodLag);
    s.rhythm = rhythmSimilarity(curve, p.rhythm);
  }

  // Ähnlichstes Störgeräusch bzw. ähnlichste Mühle (nach demselben
  // Gesamtwert wie die Pumpe).
  auto compare = [&](const NoiseProfile::Entry& e, float& sim, float& rhy) {
    float xe[cfg::kNumBands];
    sim = excessFor(e.micGain, e.bgBandDb, xe) ? bestTemplate(xe, e.templates, e.templateCount) : 0.0f;
    rhy = rhythmSimilarity(curve, e.rhythm);
    return combined(p.periodic, sim, rhy);
  };
  float bestNoise = -2.0f;
  for (size_t i = 0; i < n.count; ++i) {
    float ns, nr;
    const float c = compare(n.entries[i], ns, nr);
    if (c > bestNoise) {
      bestNoise = c;
      s.noiseSimilarity = ns;
      s.noiseRhythm = nr;
    }
  }
  float bestGrinder = -2.0f;
  for (size_t i = 0; g && i < g->count; ++i) {
    float gs, gr;
    const float c = compare(g->entries[i], gs, gr);
    if (c > bestGrinder) {
      const NoiseProfile::Entry& e = g->entries[i];
      bestGrinder = c;
      s.grinderIndex = static_cast<int8_t>(i);
      s.grinderSimilarity = gs;
      s.grinderRhythm = gr;
      // Pegel relativ zum eigenen Hintergrund (0) .. eigenen Mahlpegel (1)
      s.grinderLevelRatio = (rmsDb + gainOffsetDb(e.micGain, gain) - e.bgRmsDb) /
                            fmaxf(e.levelDb - e.bgRmsDb, 3.0f);
    }
  }
  s.hasNoise = n.count > 0 || s.grinderIndex >= 0;
  s.bestIsGrinder = s.grinderIndex >= 0 && bestGrinder >= bestNoise;
  if (s.bestIsGrinder) {  // für die Ablehnung zählt das Ähnlichste von beiden
    s.noiseSimilarity = s.grinderSimilarity;
    s.noiseRhythm = s.grinderRhythm;
  }
  return s;
}

bool PumpDetector::matches(const FrameScores& s, const PumpProfile& p, const DetectorSettings& st,
                           bool hysteresis, bool* rejected) {
  if (rejected) *rejected = false;
  if (!p.valid) return false;

  float simTh = st.similarityThreshold;
  float perTh = st.periodicityThreshold;
  float rhyTh = p.rhythmThreshold;
  float lvlTh = st.levelThreshold;
  float steadyTh = p.steadyThreshold;
  if (hysteresis) {
    steadyTh *= kHystSteady;
    simTh *= kHystSimilarity;
    perTh *= kHystPeriodicity;
    rhyTh *= kHystRhythm;
    lvlTh *= kHystLevel;
  }

  // Standardprofil (ohne Klangmuster): nur Pegel und Gleichmäßigkeit zählen.
  const bool soundsLikePump = p.periodic          ? (s.periodicity >= perTh && s.rhythm >= rhyTh)
                              : p.templateCount == 0 ? true
                                                     : s.similarity >= simTh;
  if (s.levelRatio < lvlTh || s.steadiness > steadyTh || !soundsLikePump) return false;

  // Klingt es einem gelernten Störgeräusch ähnlicher als der Pumpe? -> ignorieren.
  // Standardprofil (keine Klangmuster der Pumpe zum Vergleich): nur verwerfen,
  // wenn es dem Störgeräusch deutlich ähnelt.
  const float pumpScore = p.templateCount == 0 && !p.periodic
                              ? st.similarityThreshold
                              : combined(p.periodic, s.similarity, s.rhythm);
  if (s.hasNoise && combined(p.periodic, s.noiseSimilarity, s.noiseRhythm) > pumpScore) {
    if (rejected) *rejected = true;
    return false;
  }
  return true;
}

void PumpDetector::mergeProfile(PumpProfile& into, const PumpProfile& from, String& note) {
  // Hintergrund mitteln (Pegel in dB, Spektrum in dB).
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    into.bgBandDb[b] = 0.5f * (into.bgBandDb[b] + from.bgBandDb[b]);
  }
  into.bgRmsDb = 0.5f * (into.bgRmsDb + from.bgRmsDb);

  // Referenz = leisere der beiden Aufnahmen, damit beide erkannt werden.
  into.pumpRmsDb = fminf(into.pumpRmsDb, from.pumpRmsDb);

  // Neue Muster anhängen, nahezu identische überspringen. Ist kein Platz mehr,
  // werden die ältesten Muster verdrängt.
  for (size_t t = 0; t < from.templateCount; ++t) {
    bool duplicate = false;
    for (size_t u = 0; u < into.templateCount; ++u) {
      if (dotBands(from.templates[t], into.templates[u]) > cfg::kTemplateMergeSimilarity) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    if (into.templateCount >= cfg::kMaxTemplates) {
      memmove(into.templates[0], into.templates[1],
              sizeof(into.templates[0]) * (cfg::kMaxTemplates - 1));
      --into.templateCount;
    }
    memcpy(into.templates[into.templateCount++], from.templates[t], sizeof(from.templates[t]));
  }

  // Takt/Rhythmus nur behalten, wenn beide Aufnahmen denselben Takt zeigen.
  if (into.periodic && from.periodic) {
    if (fabsf(into.periodLag - from.periodLag) > into.periodLag * cfg::kLagTolerance * 2.0f) {
      into.periodic = false;
      note = "Takt der beiden Aufnahmen unterschiedlich – Erkennung nur über Klangmuster.";
    } else {
      // Rhythmus-Fingerabdrücke mitteln, Schwelle = tolerantere der beiden.
      float sum[cfg::kAcfLen];
      for (size_t k = 0; k < cfg::kAcfLen; ++k) sum[k] = into.rhythm[k] + from.rhythm[k];
      normalizeCurve(sum, into.rhythm);
      into.rhythmThreshold = fminf(into.rhythmThreshold, from.rhythmThreshold);
    }
  } else if (into.periodic != from.periodic) {
    into.periodic = false;
    note = "Takt nur in einer Aufnahme ausgeprägt – Erkennung nur über Klangmuster.";
  }

  into.quality = fminf(into.quality, from.quality);
  ++into.recordings;
}

void PumpDetector::mergeNoise(NoiseProfile::Entry& into, const NoiseProfile::Entry& from) {
  // Hintergrund und Pegel der neuen Aufnahme auf die Verstärkung des Eintrags
  // umrechnen; Hintergrund mitteln, Pegel/Schwelle = jeweils tolerantere.
  const float off = gainOffsetDb(into.micGain, from.micGain);
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    into.bgBandDb[b] = 0.5f * (into.bgBandDb[b] + from.bgBandDb[b] + off);
  }
  into.bgRmsDb = 0.5f * (into.bgRmsDb + from.bgRmsDb + off);
  into.levelDb = fminf(into.levelDb, from.levelDb + off);
  into.simThreshold = fminf(into.simThreshold, from.simThreshold);

  // Muster anhängen, nahezu identische überspringen; ist kein Platz mehr,
  // werden die ältesten verdrängt.
  for (size_t t = 0; t < from.templateCount; ++t) {
    bool duplicate = false;
    for (size_t u = 0; u < into.templateCount && !duplicate; ++u) {
      duplicate = dotBands(from.templates[t], into.templates[u]) > cfg::kTemplateMergeSimilarity;
    }
    if (duplicate) continue;
    if (into.templateCount >= cfg::kNoiseTemplates) {
      memmove(into.templates[0], into.templates[1],
              sizeof(into.templates[0]) * (cfg::kNoiseTemplates - 1));
      --into.templateCount;
    }
    memcpy(into.templates[into.templateCount++], from.templates[t], sizeof(from.templates[t]));
  }
  // Rhythmus-Fingerabdrücke mitteln
  float sum[cfg::kAcfLen];
  for (size_t k = 0; k < cfg::kAcfLen; ++k) sum[k] = into.rhythm[k] + from.rhythm[k];
  normalizeCurve(sum, into.rhythm);
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

void PumpDetector::updateGrinder(uint32_t now) {
  const FrameScores& s = status_.scores;
  const bool on = status_.grinderOn;
  bool raw = false;
  if (s.bestIsGrinder && !status_.rawActive && !status_.pumpOn) {
    // Vergleich mit der Pumpe nur, wenn sie eingelernt ist (Muster oder Takt).
    // Ohne gelernte Pumpe entscheiden allein Schwelle und Pegel der Mühle.
    const bool pumpLearned = profile_.templateCount > 0 || profile_.periodic;
    const float pumpScore = pumpLearned ? combined(profile_.periodic, s.similarity, s.rhythm) : -2.0f;
    // Pegel und Schwelle stammen aus der Aufnahme der Mühle selbst.
    const NoiseProfile::Entry& e = grinders_->entries[s.grinderIndex];
    const float simTh = e.simThreshold * (on ? kHystSimilarity : 1.0f);
    const float lvlTh = kGrinderLevel * (on ? kHystLevel : 1.0f);
    // Eine Mühle muss besser passen als alle Störgeräusche (bestIsGrinder) und als die Pumpe.
    raw = s.grinderLevelRatio >= lvlTh && s.grinderSimilarity >= simTh &&
          combined(profile_.periodic, s.grinderSimilarity, s.grinderRhythm) >= pumpScore;
  }

  if (raw) {
    if (grinderSinceMs_ == 0) grinderSinceMs_ = now;
    grinderLastMs_ = now;
    if (!on && now - grinderSinceMs_ >= kGrinderOnMs) {
      status_.grinderOn = true;
      status_.grinderIndex = s.grinderIndex;
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
  status_.periodic = profile_.periodic;
  steady_.reset();
  loudSinceMs_ = 0;
  activeSinceMs_ = 0;
  onsetMs_ = 0;
  lastActiveMs_ = 0;
}

PumpDetector::Event PumpDetector::process(const AudioFrame& f, const float* curve) {
  // Pegel in den Einheiten des Pumpenprofils (gelernt evtl. mit anderer Verstärkung)
  const float rmsP = f.rmsDb + gainOffsetDb(profile_.micGain, gain_);
  status_.rmsDb = rmsP;
  status_.periodic = profile_.periodic;
  if (!profile_.valid) {
    status_.scores = FrameScores{};
    status_.rawActive = status_.rejected = status_.grinderOn = false;
    return Event::None;
  }

  // Gleichmäßigkeit über die aktuelle laute Phase
  const bool loud = isLoud(rmsP, profile_, settings_, status_.pumpOn);
  if (loud && steady_.runLength() == 0) loudSinceMs_ = f.timestampMs;
  const float steadiness = steady_.update(rmsP, loud);

  status_.scores = score(f.bandDb, f.rmsDb, curve, steadiness, profile_, noise_, grinders_, gain_);
  status_.rawActive = matches(status_.scores, profile_, settings_, status_.pumpOn,
                              &status_.rejected);
  updateGrinder(f.timestampMs);
  // Vibration: allein entscheidend bzw. zusätzlich erforderlich
  if (source_ == Source::Vib) {
    status_.rawActive = vibActive_;
    status_.rejected = false;
  } else if (source_ == Source::Both) {
    status_.rawActive = status_.rawActive && vibActive_;
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
        // Timer rückwirkend starten: Gleichmäßigkeit und Klangmuster brauchen
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
