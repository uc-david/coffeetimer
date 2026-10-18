// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file classifier.cpp
 * @brief Implementierung des Klassifikators (LDA mit Schrumpfung).
 */
#include "classifier.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <memory>

namespace classifier {

namespace {

constexpr size_t B = cfg::kNumBands;
/// Schrumpfung der Kovarianz Richtung Diagonale: stabil auch mit wenigen
/// Klangbildern (sonst würde die 40×40-Matrix leicht singulär).
constexpr double kShrinkage = 0.15;

bool sameClass(const Record& r, Kind kind, uint8_t index) {
  return r.kind == kind && (kind == Kind::Pump || kind == Kind::Background || r.index == index);
}

/// Löst A·x = b für eine symmetrisch positiv definite Matrix (Cholesky, in place).
bool choleskySolve(double* a, const double* b, double* x, size_t n) {
  for (size_t j = 0; j < n; ++j) {
    double d = a[j * n + j];
    for (size_t k = 0; k < j; ++k) d -= a[j * n + k] * a[j * n + k];
    if (d <= 1e-12) return false;
    a[j * n + j] = sqrt(d);
    for (size_t i = j + 1; i < n; ++i) {
      double s = a[i * n + j];
      for (size_t k = 0; k < j; ++k) s -= a[i * n + k] * a[j * n + k];
      a[i * n + j] = s / a[j * n + j];
    }
  }
  // L·y = b, dann Lᵀ·x = y
  for (size_t i = 0; i < n; ++i) {
    double s = b[i];
    for (size_t k = 0; k < i; ++k) s -= a[i * n + k] * x[k];
    x[i] = s / a[i * n + i];
  }
  for (size_t i = n; i-- > 0;) {
    double s = x[i];
    for (size_t k = i + 1; k < n; ++k) s -= a[k * n + i] * x[k];
    x[i] = s / a[i * n + i];
  }
  return true;
}

}  // namespace

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Pump:       return "Pumpe";
    case Kind::Grinder:    return "Muehle";
    case Kind::Noise:      return "Stoergeraeusch";
    case Kind::Background: return "Ruhe";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Trainingsdaten
// ---------------------------------------------------------------------------

void TrainingSet::add(Record&& r, size_t maxPerClass) {
  // Älteste Aufnahmen derselben Klasse verdrängen, wenn zu viele
  size_t same = 0;
  for (const Record& x : records) same += sameClass(x, r.kind, r.index);
  while (same >= maxPerClass) {
    auto it = std::find_if(records.begin(), records.end(),
                           [&](const Record& x) { return sameClass(x, r.kind, r.index); });
    records.erase(it);
    --same;
  }
  records.push_back(std::move(r));
}

void TrainingSet::removeKind(Kind kind) {
  records.erase(std::remove_if(records.begin(), records.end(), [&](const Record& x) { return x.kind == kind; }),
                records.end());
}

void TrainingSet::removeEntry(Kind kind, uint8_t index) {
  records.erase(std::remove_if(records.begin(), records.end(),
                               [&](const Record& x) { return x.kind == kind && x.index == index; }),
                records.end());
  for (Record& x : records) {
    if (x.kind == kind && x.index > index) --x.index;
  }
}

size_t TrainingSet::totalFrames() const {
  size_t n = 0;
  for (const Record& r : records) n += r.count();
  return n;
}

void quantizeShape(const float* bandDb, int8_t* out) {
  float mean = 0.0f;
  for (size_t b = 0; b < B; ++b) mean += bandDb[b];
  mean /= B;
  for (size_t b = 0; b < B; ++b) {
    const float q = roundf((bandDb[b] - mean) / kQuantStep);
    out[b] = static_cast<int8_t>(q < -127.0f ? -127.0f : q > 127.0f ? 127.0f : q);
  }
}

// ---------------------------------------------------------------------------
// Training (LDA): Klassenmittel, gemeinsame Kovarianz, Gewichte Σ⁻¹·μ
// ---------------------------------------------------------------------------

bool Classifier::train(const TrainingSet& set) {
  ready_ = false;
  classCount_ = 0;
  frames_ = 0;
  accuracy_ = 0.0f;

  // Klassen bestimmen
  struct Acc {
    Kind kind;
    uint8_t index;
    double sum[B];
    size_t n;
  };
  std::unique_ptr<Acc[]> acc(new Acc[kMaxClasses]);
  size_t nc = 0;
  auto classOf = [&](const Record& r) -> int {
    for (size_t c = 0; c < nc; ++c) {
      if (acc[c].kind == r.kind && (r.kind == Kind::Pump || r.kind == Kind::Background || acc[c].index == r.index)) {
        return static_cast<int>(c);
      }
    }
    return -1;
  };
  for (const Record& r : set.records) {
    if (!r.count() || classOf(r) >= 0 || nc >= kMaxClasses) continue;
    acc[nc] = Acc{r.kind, r.index, {}, 0};
    ++nc;
  }
  if (nc < 2) return false;

  // Klassenmittel
  for (const Record& r : set.records) {
    const int c = classOf(r);
    if (c < 0) continue;
    for (size_t i = 0; i < r.count(); ++i) {
      const int8_t* f = &r.frames[i * B];
      for (size_t b = 0; b < B; ++b) acc[c].sum[b] += f[b] * kQuantStep;
    }
    acc[c].n += r.count();
  }
  size_t total = 0;
  for (size_t c = 0; c < nc; ++c) {
    for (size_t b = 0; b < B; ++b) acc[c].sum[b] /= std::max<size_t>(acc[c].n, 1);
    total += acc[c].n;
  }
  if (total <= nc) return false;

  // Gemeinsame Kovarianz innerhalb der Klassen. Je Aufnahme (≤ 200 Klangbilder)
  // in float summieren – der ESP32-S3 rechnet float in Hardware, double nur in
  // Software – und nur die Zwischensummen in double übernehmen.
  std::unique_ptr<double[]> cov(new double[B * B]());
  std::unique_ptr<float[]> part(new float[B * B]);
  float x[B];
  for (const Record& r : set.records) {
    const int c = classOf(r);
    if (c < 0) continue;
    std::fill(part.get(), part.get() + B * B, 0.0f);
    for (size_t i = 0; i < r.count(); ++i) {
      const int8_t* f = &r.frames[i * B];
      for (size_t b = 0; b < B; ++b) x[b] = f[b] * kQuantStep - static_cast<float>(acc[c].sum[b]);
      for (size_t p = 0; p < B; ++p) {
        const float xp = x[p];
        float* row = &part[p * B];
        for (size_t q = 0; q <= p; ++q) row[q] += xp * x[q];
      }
    }
    for (size_t p = 0; p < B; ++p) {
      for (size_t q = 0; q <= p; ++q) cov[p * B + q] += part[p * B + q];
    }
  }
  double avgVar = 0.0;
  for (size_t p = 0; p < B; ++p) {
    for (size_t q = 0; q <= p; ++q) {
      cov[p * B + q] /= static_cast<double>(total - nc);
      cov[q * B + p] = cov[p * B + q];
    }
    avgVar += cov[p * B + p];
  }
  avgVar = std::max(avgVar / B, 1e-3);
  // Schrumpfung: (1-λ)·Σ + λ·mittlere Varianz·I
  for (size_t p = 0; p < B; ++p) {
    for (size_t q = 0; q < B; ++q) cov[p * B + q] *= (1.0 - kShrinkage);
    cov[p * B + p] += kShrinkage * avgVar;
  }

  // Gewichte je Klasse: w = Σ⁻¹·μ, b = -½·μᵀ·w (Cholesky-Zerlegung je Klasse neu)
  std::unique_ptr<double[]> work(new double[B * B]);
  double w[B];
  for (size_t c = 0; c < nc; ++c) {
    memcpy(work.get(), cov.get(), sizeof(double) * B * B);
    if (!choleskySolve(work.get(), acc[c].sum, w, B)) return false;
    ClassInfo& ci = classes_[c];
    ci.kind = acc[c].kind;
    ci.index = acc[c].index;
    double bias = 0.0;
    for (size_t b = 0; b < B; ++b) {
      ci.w[b] = static_cast<float>(w[b]);
      bias += acc[c].sum[b] * w[b];
    }
    ci.b = static_cast<float>(-0.5 * bias);
  }
  classCount_ = nc;
  frames_ = total;
  ready_ = true;

  // Trefferquote auf den eigenen Trainingsdaten
  size_t correct = 0;
  float xf[B], score[kMaxClasses];
  for (const Record& r : set.records) {
    for (size_t i = 0; i < r.count(); ++i) {
      for (size_t b = 0; b < B; ++b) xf[b] = r.frames[i * B + b] * kQuantStep;
      scores(xf, score);
      const Result res = pick(score);
      correct += res.kind == r.kind && (r.kind == Kind::Pump || r.kind == Kind::Background || res.index == r.index);
    }
  }
  accuracy_ = static_cast<float>(correct) / total;
  return true;
}

void Classifier::scores(const float* x, float* out) const {
  for (size_t c = 0; c < classCount_; ++c) {
    float s = classes_[c].b;
    for (size_t b = 0; b < B; ++b) s += classes_[c].w[b] * x[b];
    out[c] = s;
  }
}

bool Classifier::hasKind(Kind k) const {
  for (size_t c = 0; c < classCount_; ++c) {
    if (classes_[c].kind == k) return true;
  }
  return false;
}

Result Classifier::pick(const float* score) const {
  Result r;
  size_t best = 0;
  for (size_t c = 1; c < classCount_; ++c) {
    if (score[c] > score[best]) best = c;
  }
  // Wahrscheinlichkeiten (Softmax) relativ zum besten Wert -> numerisch stabil
  float sum = 0.0f, pump = 0.0f, bestP = 0.0f;
  for (size_t c = 0; c < classCount_; ++c) {
    const float e = expf(score[c] - score[best]);
    sum += e;
    if (classes_[c].kind == Kind::Pump) pump += e;
    if (c == best) bestP = e;
  }
  r.valid = true;
  r.kind = classes_[best].kind;
  r.index = classes_[best].index;
  r.probability = bestP / sum;
  r.pumpProbability = pump / sum;
  return r;
}

Result Classifier::classify(const float* bandDb) const {
  if (!ready_) return Result{};
  int8_t q[B];
  quantizeShape(bandDb, q);  // gleiche Aufbereitung wie beim Training
  float x[B], score[kMaxClasses];
  for (size_t b = 0; b < B; ++b) x[b] = q[b] * kQuantStep;
  scores(x, score);
  return pick(score);
}

}  // namespace classifier
