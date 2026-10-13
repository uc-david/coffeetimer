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

float dotBands(const float* a, const float* b) {
  float s = 0.0f;
  for (size_t i = 0; i < cfg::kNumBands; ++i) s += a[i] * b[i];
  return s;
}

/// Normiert einen Vektor auf Länge 1 (Nullvektor bleibt unverändert).
void normalize(float* v) {
  float n = sqrtf(dotBands(v, v));
  if (n < 1e-6f) return;
  for (size_t i = 0; i < cfg::kNumBands; ++i) v[i] /= n;
}

/**
 * Sphärisches k-Means: gruppiert die (normierten) Klangvektoren der Pumpe in
 * bis zu k Muster. Startwerte sind zeitlich gleichmäßig verteilte Frames –
 * so landen Anlauf, Druckaufbau und voller Druck typischerweise in eigenen
 * Gruppen.
 * @param vecs   n Vektoren à kNumBands, jeweils normiert
 * @param out    Ergebnis-Muster
 * @return Anzahl der Muster (nach Entfernen von Duplikaten / Kleinstgruppen)
 */
size_t clusterTemplates(const float* vecs, size_t n, size_t k,
                        float (*out)[cfg::kNumBands]) {
  constexpr size_t B = cfg::kNumBands;
  if (n == 0) return 0;
  k = std::max<size_t>(1, std::min(k, n / 15));

  std::vector<float> centers(k * B);
  for (size_t j = 0; j < k; ++j) {
    memcpy(&centers[j * B], &vecs[((2 * j + 1) * n / (2 * k)) * B], B * sizeof(float));
  }

  std::vector<size_t> sizes(k);
  std::vector<float> sums(k * B);
  for (int iter = 0; iter < 12; ++iter) {
    std::fill(sums.begin(), sums.end(), 0.0f);
    std::fill(sizes.begin(), sizes.end(), 0);
    for (size_t i = 0; i < n; ++i) {
      const float* v = &vecs[i * B];
      size_t best = 0;
      float bestSim = -2.0f;
      for (size_t j = 0; j < k; ++j) {
        const float s = dotBands(v, &centers[j * B]);
        if (s > bestSim) { bestSim = s; best = j; }
      }
      for (size_t b = 0; b < B; ++b) sums[best * B + b] += v[b];
      ++sizes[best];
    }
    for (size_t j = 0; j < k; ++j) {
      if (sizes[j] == 0) continue;  // leere Gruppe: alten Mittelpunkt behalten
      memcpy(&centers[j * B], &sums[j * B], B * sizeof(float));
      normalize(&centers[j * B]);
    }
  }

  // Kleinstgruppen (< 5 %, eher Störgeräusche) und Duplikate verwerfen.
  size_t count = 0;
  for (size_t j = 0; j < k; ++j) {
    if (sizes[j] < n / 20 + 1) continue;
    bool dup = false;
    for (size_t u = 0; u < count; ++u) {
      if (dotBands(&centers[j * B], out[u]) > cfg::kTemplateMergeSimilarity) { dup = true; break; }
    }
    if (dup) continue;
    memcpy(out[count++], &centers[j * B], B * sizeof(float));
  }
  return count;
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
  // Pumpe: bis 60 s ≈ 1900 Frames -> ~210 kB Merkmale + ~850 kB Autokorrelation.
  bgFrames_ = static_cast<AudioFrame*>(allocLarge(sizeof(AudioFrame) * cfg::kTeachMaxBgFrames));
  bgCurves_ = static_cast<int8_t*>(allocLarge(cfg::kAcfLen * cfg::kTeachMaxBgFrames));
  pumpFrames_ = static_cast<AudioFrame*>(allocLarge(sizeof(AudioFrame) * cfg::kTeachMaxPumpFrames));
  pumpCurves_ = static_cast<int8_t*>(allocLarge(cfg::kAcfLen * cfg::kTeachMaxPumpFrames));
}

void TeachController::start(uint32_t nowMs, TeachMode mode, const DetectorSettings& current,
                            const PumpProfile& profile, const NoiseProfile& noise) {
  mode_ = mode;
  clearResult();
  settings_ = current;
  currentProfile_ = profile;
  currentNoise_ = noise;
  bgCount_ = 0;
  pumpCount_ = 0;
  triggerFrames_ = 0;
  recordedMs_ = 0;
  finishRequested_ = false;

  if (mode_ == TeachMode::Pump) {
    bgRmsDb_ = -90.0f;
    enterPhase(TeachPhase::Background, nowMs);
  } else {
    // Störgeräusch: Hintergrund ist aus dem Pumpenprofil bekannt.
    bgRmsDb_ = profile.bgRmsDb;
    enterPhase(TeachPhase::WaitPump, nowMs);
    if (!profile.valid) fail("Bitte zuerst die Pumpe einlernen – Störgeräusche werden mit ihr verglichen.");
  }
  if (!bgFrames_ || !bgCurves_ || !pumpFrames_ || !pumpCurves_) {
    fail("Nicht genug Speicher für die Aufnahme (PSRAM).");
  }
}

void TeachController::clearResult() {
  // Kopie aus einem statischen Leerobjekt: TeachResult ist ~9 kB groß und
  // soll nicht als Temporärobjekt auf dem Stack landen.
  static const TeachResult kEmpty;
  result_ = kEmpty;
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
  if (mode_ == TeachMode::Noise) {
    switch (phase_) {
      case TeachPhase::WaitPump:
        return "Schritt 1/2: Jetzt das Störgeräusch einschalten (z. B. Mühle mahlen lassen).";
      case TeachPhase::RecordPump:
        return "Schritt 2/2: Aufnahme läuft – Gerät normal laufen lassen, danach ausschalten.";
      case TeachPhase::Done:
        return result_.quality == TeachQuality::Error
                   ? "Aufnahme nicht verwendbar – bitte wiederholen."
                   : "Aufnahme fertig – Störgeräusch speichern?";
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
      return result_.quality == TeachQuality::Error
                 ? "Aufnahme nicht verwendbar – bitte wiederholen."
                 : "Aufnahme fertig – Profil ersetzen oder ergänzen.";
  }
  return "";
}

void TeachController::storeCurve(int8_t* dst, const float* curve) {
  for (size_t i = 0; i < cfg::kAcfLen; ++i) {
    const float r = constrain(curve[i], -1.0f, 1.0f);
    dst[i] = static_cast<int8_t>(lroundf(r * 127.0f));
  }
}

void TeachController::loadCurve(const int8_t* src, float* curve) {
  for (size_t i = 0; i < cfg::kAcfLen; ++i) curve[i] = src[i] / 127.0f;
}

void TeachController::process(const AudioFrame& f, const float* curve) {
  if (!recording()) return;

  liveRmsDb_ = f.rmsDb;
  const uint32_t elapsed = f.timestampMs - phaseStartMs_;

  switch (phase_) {
    case TeachPhase::Background: {
      // Erste Millisekunden verwerfen: Geräusch der Touch-Berührung o. ä.
      if (elapsed >= cfg::kTeachBackgroundSkipMs && bgCount_ < cfg::kTeachMaxBgFrames) {
        bgFrames_[bgCount_] = f;
        storeCurve(&bgCurves_[bgCount_ * cfg::kAcfLen], curve);
        ++bgCount_;
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
        result_.addMessage(mode_ == TeachMode::Pump
            ? "Tipp: Gerät näher an die Maschine stellen (Pumpe sitzt meist unten/hinten) und "
              "die Pumpe während Schritt 2 einschalten."
            : "Tipp: Das Störgeräusch während Schritt 1 einschalten. Ist es sehr leise, stört "
              "es die Erkennung ohnehin nicht.");
      }
      break;
    }

    case TeachPhase::RecordPump: {
      recordedMs_ = elapsed;
      if (elapsed >= cfg::kTeachPumpSkipMs && pumpCount_ < cfg::kTeachMaxPumpFrames) {
        pumpFrames_[pumpCount_] = f;
        storeCurve(&pumpCurves_[pumpCount_ * cfg::kAcfLen], curve);
        ++pumpCount_;
      }
      // Ende erkennen: Pumpe lange genug still (Pausen der Pre-Infusion
      // werden überbrückt), Puffer voll oder Maximaldauer erreicht.
      if (f.rmsDb >= bgRmsDb_ + cfg::kTeachActiveDb) {
        lastLoudMs_ = f.timestampMs;
        lastLoudIndex_ = pumpCount_;
      }
      // Störgeräusche (Mühle) haben keine Pre-Infusion -> kürzere Stille genügt.
      const bool noise = mode_ == TeachMode::Noise;
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
         "Schritt 1. Pumpe aus? Bitte in Ruhe wiederholen.", clip * 100.0f);
  }
}

void TeachController::analyzePump() {
  constexpr size_t B = cfg::kNumBands;
  constexpr size_t L = cfg::kAcfLen;
  static const NoiseProfile kNoNoise;  // Bewertung zunächst ohne Störgeräusche
  TeachResult& r = result_;
  clearResult();
  PumpProfile& p = r.profile;

  // Stille am Ende (Pumpe bereits aus) abschneiden.
  pumpCount_ = std::min(pumpCount_, lastLoudIndex_);

  // =========================================================================
  // 1. Hintergrund
  // =========================================================================
  rmsStats(bgFrames_, bgCount_, p.bgRmsDb, r.bgStdDb);
  for (size_t b = 0; b < B; ++b) {
    double sum = 0.0;
    for (size_t i = 0; i < bgCount_; ++i) sum += bgFrames_[i].bandDb[b];
    p.bgBandDb[b] = static_cast<float>(sum / std::max<size_t>(bgCount_, 1));
  }

  // =========================================================================
  // 2. Hörbare Pumpen-Frames bestimmen, Pausen messen
  // =========================================================================
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

  if (active.size() * cfg::kFrameSize * 1000 / cfg::kSampleRate < cfg::kTeachPumpMinActiveMs) {
    fail("Pumpe nur %.1f s hörbar (mind. %.0f s nötig). Bezug länger laufen lassen bzw. "
         "Gerät näher an die Pumpe stellen.",
         r.activeSeconds, cfg::kTeachPumpMinActiveMs / 1000.0f);
    return;
  }

  // =========================================================================
  // 3. Pegel: Referenz = leiser Teil der Pumpe
  // =========================================================================
  {
    std::vector<float> levels;
    levels.reserve(active.size());
    for (size_t i : active) levels.push_back(pumpFrames_[i].rmsDb);
    p.pumpRmsDb = percentile(levels, 0.10f);
  }
  r.snrDb = p.pumpRmsDb - p.bgRmsDb;

  // =========================================================================
  // 4. Klangmuster (Clustering der Überschuss-Spektren)
  // =========================================================================
  float* vecs = static_cast<float*>(allocLarge(active.size() * B * sizeof(float)));
  if (!vecs) {
    fail("Nicht genug Speicher für die Auswertung.");
    return;
  }
  size_t nVecs = 0;
  double meanExcess[B] = {};
  for (size_t i : active) {
    for (size_t b = 0; b < B; ++b) meanExcess[b] += pumpFrames_[i].bandDb[b] - p.bgBandDb[b];
    if (PumpDetector::excessVector(pumpFrames_[i].bandDb, p.bgBandDb, &vecs[nVecs * B]) > 0.0f) {
      ++nVecs;
    }
  }
  for (size_t b = 0; b < B; ++b) {
    if (meanExcess[b] / active.size() >= cfg::kQualDistinctBandDb) ++r.distinctBands;
  }
  p.templateCount = static_cast<uint8_t>(
      clusterTemplates(vecs, nVecs, cfg::kTemplatesPerTeach, p.templates));
  free(vecs);
  p.valid = p.templateCount > 0;
  p.recordings = 1;

  // Klangfarben-Kontrast: Wie stark unterscheiden sich die Bänder? Ist die
  // Pumpe in allen Bändern etwa gleich viel lauter als der Hintergrund
  // (rauschartig), gibt es kaum eine Klangfarbe, an der man sie erkennt.
  {
    double mean = 0.0, sq = 0.0;
    for (size_t b = 0; b < B; ++b) {
      const double v = meanExcess[b] / active.size();
      mean += v;
      sq += v * v;
    }
    mean /= B;
    r.contrastDb = static_cast<float>(sqrt(std::max(0.0, sq / B - mean * mean)));
  }
  const bool broadband = r.contrastDb < 2.0f;

  // Gleichmäßigkeit je Frame (in zeitlicher Reihenfolge, wie im Betrieb)
  std::vector<float> pumpSteady(pumpCount_), bgSteady(bgCount_);
  {
    SteadinessTracker tr;
    for (size_t i = 0; i < pumpCount_; ++i) {
      pumpSteady[i] = tr.update(pumpFrames_[i].rmsDb,
                                PumpDetector::isLoud(pumpFrames_[i].rmsDb, p, settings_, false));
    }
    tr.reset();
    for (size_t i = 0; i < bgCount_; ++i) {
      bgSteady[i] = tr.update(bgFrames_[i].rmsDb,
                              PumpDetector::isLoud(bgFrames_[i].rmsDb, p, settings_, false));
    }
    // Schwelle: deutlich über der Schwankung der Pumpe (95 %), mind. 1,5 dB.
    std::vector<float> valid;
    for (size_t i : active) {
      if (pumpSteady[i] < SteadinessTracker::kUnsteady) valid.push_back(pumpSteady[i]);
    }
    r.pumpSteadiness = valid.size() >= 10 ? percentile(valid, 0.95f) : 4.0f;
    p.steadyThreshold = constrain(r.pumpSteadiness * 1.5f + 0.5f, 1.5f, 6.0f);
  }

  // =========================================================================
  // 5. Takt und Rhythmus-Fingerabdruck (Autokorrelation der Hüllkurve)
  // =========================================================================
  float curve[L];
  std::vector<float> meanAcf(L, 0.0f);
  for (size_t i : active) {
    loadCurve(&pumpCurves_[i * L], curve);
    for (size_t k = 0; k < L; ++k) meanAcf[k] += curve[k];
  }
  for (float& v : meanAcf) v /= active.size();
  // Leicht geglättete Kopie für die Spitzensuche (Spitzen der Hüllkurven-
  // Autokorrelation sind breit und haben kleine Zacken).
  std::vector<float> smooth(cfg::kAcfLen, 0.0f);
  float maxAcf = -1.0f;
  for (size_t k = 0; k < cfg::kAcfLen; ++k) {
    float sum = 0.0f;
    int n = 0;
    for (int d = -3; d <= 3; ++d) {
      const int j = static_cast<int>(k) + d;
      if (j >= 0 && j < (int)cfg::kAcfLen) { sum += meanAcf[j]; ++n; }
    }
    smooth[k] = sum / n;
    maxAcf = std::max(maxAcf, smooth[k]);
  }
  // Kleinste Taktlänge, deren Spitze fast so hoch ist wie die höchste:
  // vermeidet, dass ein Vielfaches des Takts (z. B. 25 statt 50 Hz) gewählt wird.
  size_t lagIdx = 0;
  for (size_t k = 1; k + 1 < cfg::kAcfLen; ++k) {
    if (smooth[k] >= smooth[k - 1] && smooth[k] >= smooth[k + 1] && smooth[k] >= 0.9f * maxAcf) {
      lagIdx = k;
      break;
    }
  }
  // Feinsuche: echtes Maximum im Umkreis von ±8 % (Spitzenmitte statt Flanke).
  {
    const float center = static_cast<float>(cfg::kMinLag + lagIdx);
    const int lo = std::max(0, static_cast<int>(center * 0.92f) - (int)cfg::kMinLag);
    const int hi = std::min((int)cfg::kAcfLen - 1, static_cast<int>(center * 1.08f) - (int)cfg::kMinLag);
    for (int k = lo; k <= hi; ++k) {
      if (smooth[k] > smooth[lagIdx]) lagIdx = k;
    }
  }
  const float lag = static_cast<float>(cfg::kMinLag + lagIdx);

  // Rhythmus-Fingerabdruck = Form der mittleren Kurve. Vorläufig Takt
  // annehmen, damit score() Takt-Stärke und Rhythmus-Form berechnet.
  PumpDetector::normalizeCurve(meanAcf.data(), p.rhythm);
  p.periodic = true;
  p.periodLag = lag;

  // Vergleichswerte aller Frames mit dem neuen Profil
  std::vector<FrameScores> pumpScores, bgScores;
  pumpScores.reserve(active.size());
  for (size_t i : active) {
    loadCurve(&pumpCurves_[i * L], curve);
    pumpScores.push_back(PumpDetector::score(pumpFrames_[i].bandDb, pumpFrames_[i].rmsDb, curve,
                                             pumpSteady[i], p, kNoNoise));
  }
  for (size_t i = 0; i < bgCount_; ++i) {
    loadCurve(&bgCurves_[i * L], curve);
    bgScores.push_back(PumpDetector::score(bgFrames_[i].bandDb, bgFrames_[i].rmsDb, curve,
                                           bgSteady[i], p, kNoNoise));
  }
  auto collect = [](const std::vector<FrameScores>& v, float FrameScores::*field) {
    std::vector<float> out;
    out.reserve(v.size());
    for (const FrameScores& s : v) out.push_back(s.*field);
    return out;
  };
  std::vector<float> tmp = collect(pumpScores, &FrameScores::periodicity);
  r.pumpPeriodicity = percentile(tmp, 0.10f);
  const float pumpPer25 = percentile(tmp, 0.25f);
  tmp = collect(bgScores, &FrameScores::periodicity);
  r.bgPeriodicity = percentile(tmp, 0.90f);
  tmp = collect(pumpScores, &FrameScores::rhythm);
  r.pumpRhythm = percentile(tmp, 0.05f);
  tmp = collect(bgScores, &FrameScores::rhythm);
  r.bgRhythm = percentile(tmp, 0.90f);

  p.periodic = maxAcf > 0.0f && pumpPer25 >= cfg::kQualMinPeriodicity &&
               pumpPer25 - r.bgPeriodicity >= cfg::kQualMinPeriodicGap;
  if (!p.periodic) p.periodLag = 0.0f;

  // Rhythmus-Schwelle: knapp unter dem schwächsten Teil der Pumpe, möglichst
  // deutlich über dem Hintergrund.
  float rhyTh = r.pumpRhythm - 0.05f;
  if (rhyTh < r.bgRhythm + 0.1f) rhyTh = 0.5f * (r.pumpRhythm + r.bgRhythm);
  p.rhythmThreshold = constrain(rhyTh, 0.2f, 0.9f);

  std::vector<float> pumpSim = collect(pumpScores, &FrameScores::similarity);
  std::vector<float> bgSim = collect(bgScores, &FrameScores::similarity);

  // =========================================================================
  // 6. Schwellen aus der Aufnahme ableiten
  // =========================================================================
  tmp = pumpSim;
  r.selfSimilarity = percentile(tmp, 0.05f);
  tmp = bgSim;
  r.bgSimilarity = percentile(tmp, 0.90f);

  // Klangmuster: knapp unter dem schwächsten Teil der Pumpe, aber möglichst
  // über dem Hintergrund. Überlappen beide, wird die Mitte genommen.
  bool spectralOverlap = false;
  float simTh = r.selfSimilarity - 0.03f;
  if (simTh < r.bgSimilarity + 0.05f) {
    spectralOverlap = true;
    simTh = 0.5f * (r.selfSimilarity + r.bgSimilarity);
  }
  r.suggestedSimilarity = constrain(simTh, 0.4f, 0.95f);

  // Takt: in der Mitte zwischen Hintergrund und schwachem Teil der Pumpe.
  r.suggestedPeriodicity = constrain(
      std::max(r.bgPeriodicity + 0.05f, 0.5f * (r.pumpPeriodicity + r.bgPeriodicity)), 0.1f, 0.9f);

  // Ausschaltverzögerung muss die längste Pause (Pre-Infusion) überbrücken.
  r.suggestedOffDelayMs = settings_.offDelayMs;
  if (r.longestPauseMs + 400 > settings_.offDelayMs) {
    r.suggestedOffDelayMs = static_cast<uint16_t>(std::min<uint32_t>(r.longestPauseMs + 400, 5000));
  }

  // =========================================================================
  // 7. Probe-Erkennung: Wie viel der Aufnahme würde damit erkannt?
  // =========================================================================
  // Gleiche Entscheidung wie im Betrieb (PumpDetector::matches).
  DetectorSettings eval = settings_;
  eval.similarityThreshold = r.suggestedSimilarity;
  eval.periodicityThreshold = r.suggestedPeriodicity;
  size_t hits = 0, rejected = 0;
  for (size_t j = 0; j < active.size(); ++j) {
    if (!PumpDetector::matches(pumpScores[j], p, kNoNoise, eval, false)) continue;
    ++hits;
    // Würde ein bereits gelerntes Störgeräusch diesen Frame verwerfen?
    if (currentNoise_.count > 0) {
      const size_t i = active[j];
      loadCurve(&pumpCurves_[i * L], curve);
      const FrameScores sn = PumpDetector::score(pumpFrames_[i].bandDb, pumpFrames_[i].rmsDb,
                                                 curve, pumpSteady[i], p, currentNoise_);
      bool rej = false;
      PumpDetector::matches(sn, p, currentNoise_, eval, false, &rej);
      if (rej) ++rejected;
    }
  }
  r.coverage = static_cast<float>(hits) / active.size();
  r.rejectedRatio = static_cast<float>(rejected) / active.size();
  size_t falseHits = 0;
  for (size_t i = 0; i < bgCount_; ++i) {
    if (PumpDetector::matches(bgScores[i], p, kNoNoise, eval, false)) ++falseHits;
  }
  r.falseRatio = bgCount_ ? static_cast<float>(falseHits) / bgCount_ : 0.0f;

  // =========================================================================
  // 8. Bewertung
  // =========================================================================
  bool error = false, warning = false;

  // --- Fehler (Profil unbrauchbar) ---
  if (r.clipRatio > cfg::kQualMaxClipRatio) {
    error = true;
    r.addMessage("Übersteuert: %.1f %% der Samples am Anschlag. Gerät weiter von der "
                 "Pumpe entfernt aufstellen.", r.clipRatio * 100.0f);
  }
  if (r.snrDb < cfg::kQualMinSnrDb) {
    error = true;
    r.addMessage("Pumpe im leisen Teil nur %.1f dB über dem Hintergrund (mind. %.0f dB). "
                 "Näher an die Pumpe oder Störquellen (Radio, Mühle, Gespräche) abstellen.",
                 r.snrDb, cfg::kQualMinSnrDb);
  }
  if (!p.valid || (r.distinctBands == 0 && !p.periodic)) {
    error = true;
    r.addMessage("Weder typisches Klangmuster noch Takt erkennbar – die Pumpe ist vom "
                 "Hintergrund nicht unterscheidbar.");
  }
  if (r.coverage < cfg::kQualCoverageErr) {
    error = true;
    r.addMessage("Nur %.0f %% der Aufnahme würden als Pumpe erkannt. Klang zu "
                 "unregelmäßig oder Störgeräusche während der Aufnahme.", r.coverage * 100.0f);
  }
  if (r.falseRatio > cfg::kQualMaxFalseRatio) {
    error = true;
    r.addMessage("Der Hintergrund würde zu %.0f %% als Pumpe erkannt – Hintergrund zu laut "
                 "oder ähnlich. In ruhiger Umgebung wiederholen.", r.falseRatio * 100.0f);
  }

  // --- Warnungen (nutzbar, aber evtl. unzuverlässig) ---
  if (!error && r.snrDb < cfg::kQualGoodSnrDb) {
    warning = true;
    r.addMessage("Geringer Pegelabstand im leisen Teil (%.1f dB). Erkennung kann bei "
                 "Nebengeräuschen unzuverlässig sein.", r.snrDb);
  }
  if (!error && r.coverage < cfg::kQualCoverageWarn) {
    warning = true;
    r.addMessage("%.0f %% der Aufnahme werden erkannt – kurze Aussetzer möglich. Profil "
                 "ggf. mit weiterer Aufnahme ergänzen.", r.coverage * 100.0f);
  }
  if (r.bgStdDb > cfg::kQualBgStdWarnDb) {
    warning = true;
    r.addMessage("Hintergrund war unruhig (±%.1f dB). Teach am besten in ruhiger Umgebung "
                 "wiederholen.", r.bgStdDb);
  }
  if (!error && spectralOverlap && !p.periodic) {
    warning = true;
    r.addMessage("Klangmuster hebt sich wenig vom Hintergrund ab – Erkennung stützt sich "
                 "vor allem auf den Pegel.");
  }
  if (r.rejectedRatio > 0.05f) {
    warning = true;
    r.addMessage("%.0f %% der Pumpe würden als gelerntes Störgeräusch verworfen – "
                 "Störgeräusche ggf. löschen und neu einlernen.", r.rejectedRatio * 100.0f);
  }
  if (!error && !p.periodic && broadband) {
    warning = true;
    r.addMessage("Klangfarbe kaum ausgeprägt (rauschartig, Kontrast %.1f dB) – ähnliche Geräusche "
                 "ggf. als Störgeräusch einlernen.", r.contrastDb);
  }

  // --- Hinweise (informativ) ---
  if (p.periodic) {
    r.addMessage("Pumpentakt %.0f Hz erkannt (typisch Vibrationspumpe) – Erkennung über Takt "
                 "und Rhythmus-Form, unabhängig vom Klang (Leerbezug wie echter Bezug).",
                 p.periodHz());
  } else if (!error) {
    r.addMessage("Kein ausgeprägter Takt (typisch Rotationspumpe) – Erkennung über "
                 "Klangmuster.");
  }
  if (!error) {
    r.addMessage("Pumpe läuft gleichmäßig (±%.1f dB) – unruhigere Geräusche über ±%.1f dB "
                 "(Niesen, Sprechen, Klappern) werden ignoriert.",
                 r.pumpSteadiness, p.steadyThreshold);
    r.addMessage("%u Klangmuster aus %.1f s Pumpenlauf gelernt.", (unsigned)p.templateCount,
                 r.activeSeconds);
  }
  if (r.suggestedOffDelayMs > settings_.offDelayMs) {
    r.addMessage("Pause von %.1f s erkannt (Pre-Infusion?) – Ausschaltverzögerung wird auf "
                 "%.1f s gesetzt.", r.longestPauseMs / 1000.0f, r.suggestedOffDelayMs / 1000.0f);
  }

  // --- Gesamtnote 0..100 ---
  const float separation = std::max(ramp(r.selfSimilarity - r.bgSimilarity, 0.0f, 0.3f),
                                     p.periodic ? ramp(r.pumpPeriodicity - r.bgPeriodicity, 0.1f, 0.4f) : 0.0f);
  float score = 35.0f * ramp(r.snrDb, cfg::kQualMinSnrDb, 20.0f) +
                30.0f * ramp(r.coverage, cfg::kQualCoverageErr, 1.0f) +
                20.0f * separation +
                10.0f * (1.0f - ramp(r.bgStdDb, 1.0f, 6.0f)) +
                5.0f * (r.clipRatio <= cfg::kQualMaxClipRatio ? 1.0f : 0.0f);
  if (error) score = fminf(score, 30.0f);
  r.score = static_cast<int>(lroundf(score));
  p.quality = static_cast<float>(r.score);

  r.quality = error ? TeachQuality::Error : (warning ? TeachQuality::Warning : TeachQuality::Ok);
  if (r.quality == TeachQuality::Ok) {
    // Positive Zusammenfassung an den Anfang stellen.
    char summary[TeachResult::kMessageLen];
    snprintf(summary, sizeof(summary), "Aufnahme gut: Pumpe %.1f dB über Hintergrund, "
             "%.0f %% der Aufnahme erkannt.", r.snrDb, r.coverage * 100.0f);
    if (r.messageCount < TeachResult::kMaxMessages) {
      memmove(r.messages[1], r.messages[0], TeachResult::kMessageLen * r.messageCount);
      ++r.messageCount;
    }
    strncpy(r.messages[0], summary, TeachResult::kMessageLen);
  }

  Serial.printf("[teach] snr=%.1f cov=%.2f false=%.2f sim(p5)=%.2f bgSim(p90)=%.2f "
                "per=%d lag=%.0f per(p10)=%.2f bgPer(p90)=%.2f rhy(p5)=%.2f bgRhy(p90)=%.2f "
                "rhyTh=%.2f steady(p95)=%.2f steadyTh=%.2f contrast=%.1f templates=%u "
                "rejected=%.2f score=%d\n",
                r.snrDb, r.coverage, r.falseRatio, r.selfSimilarity, r.bgSimilarity,
                p.periodic, lag, r.pumpPeriodicity, r.bgPeriodicity, r.pumpRhythm, r.bgRhythm,
                p.rhythmThreshold, r.pumpSteadiness, p.steadyThreshold, r.contrastDb,
                p.templateCount, r.rejectedRatio, r.score);

  phase_ = TeachPhase::Done;
  progress_ = 1.0f;
}

void TeachController::analyzeNoise() {
  constexpr size_t B = cfg::kNumBands;
  constexpr size_t L = cfg::kAcfLen;
  TeachResult& r = result_;
  clearResult();
  const PumpProfile& p = currentProfile_;
  NoiseProfile::Entry& e = r.noise;

  // Stille am Ende abschneiden, hörbare Frames bestimmen.
  pumpCount_ = std::min(pumpCount_, lastLoudIndex_);
  const float activeLimit = p.bgRmsDb + cfg::kTeachActiveDb;
  std::vector<size_t> active;
  for (size_t i = 0; i < pumpCount_; ++i) {
    if (pumpFrames_[i].rmsDb >= activeLimit) active.push_back(i);
  }
  r.activeSeconds = active.size() * cfg::kFrameSize / static_cast<float>(cfg::kSampleRate);
  r.clipRatio = meanClip(pumpFrames_, pumpCount_);
  if (r.activeSeconds * 1000.0f < cfg::kTeachNoiseMinActiveMs) {
    fail("Störgeräusch nur %.1f s hörbar (mind. %.0f s nötig). Länger laufen lassen.",
         r.activeSeconds, cfg::kTeachNoiseMinActiveMs / 1000.0f);
    return;
  }

  // --- Klangmuster und Rhythmus-Fingerabdruck des Störgeräuschs -------------
  float* vecs = static_cast<float*>(allocLarge(active.size() * B * sizeof(float)));
  if (!vecs) {
    fail("Nicht genug Speicher für die Auswertung.");
    return;
  }
  size_t nVecs = 0;
  float curve[L];
  std::vector<float> meanCurve(L, 0.0f);
  for (size_t i : active) {
    if (PumpDetector::excessVector(pumpFrames_[i].bandDb, p.bgBandDb, &vecs[nVecs * B]) > 0.0f) {
      ++nVecs;
    }
    loadCurve(&pumpCurves_[i * L], curve);
    for (size_t k = 0; k < L; ++k) meanCurve[k] += curve[k];
  }
  e.templateCount = static_cast<uint8_t>(
      clusterTemplates(vecs, nVecs, cfg::kNoiseTemplates, e.templates));
  free(vecs);
  PumpDetector::normalizeCurve(meanCurve.data(), e.rhythm);

  // --- Wie oft wird es bisher bzw. künftig als Pumpe erkannt? ---------------
  // Statisch: NoiseProfile ist ~6 kB groß.
  static NoiseProfile extended;
  extended = currentNoise_;
  if (extended.count >= cfg::kMaxNoises) {
    memmove(&extended.entries[0], &extended.entries[1],
            sizeof(NoiseProfile::Entry) * (cfg::kMaxNoises - 1));
    --extended.count;
  }
  extended.entries[extended.count++] = e;

  // Gleichmäßigkeit wie im Betrieb (zeitliche Reihenfolge aller Frames)
  std::vector<float> steady(pumpCount_);
  {
    SteadinessTracker tr;
    for (size_t i = 0; i < pumpCount_; ++i) {
      steady[i] = tr.update(pumpFrames_[i].rmsDb,
                            PumpDetector::isLoud(pumpFrames_[i].rmsDb, p, settings_, false));
    }
  }

  size_t before = 0, after = 0;
  for (size_t i : active) {
    loadCurve(&pumpCurves_[i * L], curve);
    const AudioFrame& f = pumpFrames_[i];
    FrameScores s = PumpDetector::score(f.bandDb, f.rmsDb, curve, steady[i], p, currentNoise_);
    if (PumpDetector::matches(s, p, currentNoise_, settings_, false)) ++before;
    s = PumpDetector::score(f.bandDb, f.rmsDb, curve, steady[i], p, extended);
    if (PumpDetector::matches(s, p, extended, settings_, false)) ++after;
  }
  r.noiseHitBefore = static_cast<float>(before) / active.size();
  r.noiseHitAfter = static_cast<float>(after) / active.size();

  // Ähnlichkeit Störgeräusch <-> Pumpe: Ist sie sehr hoch, könnte das neue
  // Störgeräusch auch die echte Pumpe unterdrücken.
  float specSim = 0.0f;
  for (size_t t = 0; t < e.templateCount; ++t) {
    specSim = std::max(specSim, PumpDetector::bestTemplate(e.templates[t], p.templates, p.templateCount));
  }
  float rhySim = 0.0f;
  for (size_t k = 0; k < L; ++k) rhySim += e.rhythm[k] * p.rhythm[k];
  const bool similarToPump = p.periodic ? (rhySim > 0.9f && specSim > 0.9f) : specSim > 0.95f;

  // --- Bewertung -------------------------------------------------------------
  bool error = false, warning = false;
  if (r.clipRatio > cfg::kQualMaxClipRatio) {
    error = true;
    r.addMessage("Übersteuert (%.1f %% der Samples) – Gerät weiter vom Störgeräusch entfernt "
                 "aufnehmen.", r.clipRatio * 100.0f);
  }
  if (!error) {
    r.addMessage("Bisher zu %.0f %% als Pumpe erkannt – mit diesem Störgeräusch: %.0f %%.",
                 r.noiseHitBefore * 100.0f, r.noiseHitAfter * 100.0f);
  }
  if (!error && r.noiseHitBefore < 0.02f) {
    r.addMessage("Wurde ohnehin nicht als Pumpe erkannt – Speichern ist nicht nötig, schadet "
                 "aber nicht.");
  }
  if (!error && r.noiseHitAfter > cfg::kQualMaxNoiseHit) {
    warning = true;
    r.addMessage("Wird teilweise weiterhin als Pumpe erkannt. Ggf. Pegel-Schwelle erhöhen "
                 "oder die Pumpe neu einlernen.");
  }
  if (similarToPump) {
    warning = true;
    r.addMessage("Klingt der Pumpe sehr ähnlich (Muster %.0f %%, Rhythmus %.0f %%) – die echte "
                 "Pumpe könnte ebenfalls ignoriert werden. Nach dem Speichern testen!",
                 specSim * 100.0f, rhySim * 100.0f);
  }
  r.addMessage("%.1f s aufgenommen, %u Klangmuster.", r.activeSeconds, (unsigned)e.templateCount);

  float score = 100.0f * (1.0f - r.noiseHitAfter) - (similarToPump ? 30.0f : 0.0f);
  if (error) score = fminf(score, 30.0f);
  r.score = constrain(static_cast<int>(lroundf(score)), 0, 100);
  r.quality = error ? TeachQuality::Error : (warning ? TeachQuality::Warning : TeachQuality::Ok);

  Serial.printf("[teach-noise] aktiv=%.1fs vorher=%.2f nachher=%.2f spec=%.2f rhy=%.2f\n",
                r.activeSeconds, r.noiseHitBefore, r.noiseHitAfter, specSim, rhySim);

  phase_ = TeachPhase::Done;
  progress_ = 1.0f;
}
