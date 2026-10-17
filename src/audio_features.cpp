/**
 * @file audio_features.cpp
 * @brief Implementierung der FFT-basierten Merkmalsextraktion.
 */
#include "audio_features.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kEps = 1e-12f;           // verhindert log10(0)
constexpr float kPreEmphasis = 0.9f;     // Höhenanhebung vor der Hüllkurve
// Glättung der Hüllkurve: 1 - exp(-2π·500 Hz / 16 kHz) -> Grenzfrequenz ~500 Hz,
// lässt Takte bis 250 Hz (kMinLag) sauber durch.
constexpr float kEnvelopeAlpha = 0.18f;
}  // namespace

void FeatureExtractor::begin() {
  const size_t n = cfg::kWindowSize;

  // Hann-Fenster reduziert den Leck-Effekt der FFT an den Fenstergrenzen.
  for (size_t i = 0; i < n; ++i) {
    window_[i] = 0.5f * (1.0f - cosf(2.0f * kPi * i / (n - 1)));
    history_[i] = 0.0f;
  }

  // Bandgrenzen logarithmisch zwischen kBandMinHz und kBandMaxHz verteilen.
  // Jedes Band erhält mindestens ein FFT-Bin, damit tiefe Bänder nicht leer sind.
  const float binHz = static_cast<float>(cfg::kSampleRate) / n;
  const float ratio = powf(cfg::kBandMaxHz / cfg::kBandMinHz, 1.0f / cfg::kNumBands);
  float f = cfg::kBandMinHz;
  uint16_t prev = static_cast<uint16_t>(lroundf(cfg::kBandMinHz / binHz));
  bandStart_[0] = prev;
  for (size_t b = 1; b <= cfg::kNumBands; ++b) {
    f *= ratio;
    uint16_t bin = static_cast<uint16_t>(lroundf(f / binHz));
    if (bin <= prev) bin = prev + 1;
    if (bin > n / 2) bin = n / 2;
    bandStart_[b] = bin;
    prev = bin;
  }
  primed_ = false;
}

void FeatureExtractor::process(const int16_t* samples, AudioFrame& out) {
  const size_t w = cfg::kWindowSize;
  const size_t hop = cfg::kFrameSize;

  // --- Fenster nachschieben und Übersteuerung im neuen Block zählen --------
  memmove(history_, history_ + hop, (w - hop) * sizeof(float));
  uint32_t clipped = 0;
  for (size_t i = 0; i < hop; ++i) {
    if (abs(samples[i]) >= cfg::kClipLevel) ++clipped;
    history_[w - hop + i] = samples[i] / 32768.0f;  // normiert auf -1..1
  }
  out.clipRatio = static_cast<float>(clipped) / hop;

  // --- Gleichanteil und Leistung des Fensters --------------------------------
  double sum = 0.0;
  for (size_t i = 0; i < w; ++i) sum += history_[i];
  const float dc = static_cast<float>(sum / w);
  double sq = 0.0;
  for (size_t i = 0; i < w; ++i) {
    const float v = history_[i] - dc;
    sq += v * v;
  }
  const float power = static_cast<float>(sq / w);

  // --- Spektrum (Hann-Fenster, FFT über das 64-ms-Fenster) --------------------
  for (size_t i = 0; i < w; ++i) {
    re_[i] = (history_[i] - dc) * window_[i];
    im_[i] = 0.0f;
  }
  fft(w);
  float bandPower[cfg::kNumBands];
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    float p = 0.0f;
    const uint16_t from = bandStart_[b];
    const uint16_t to = bandStart_[b + 1];
    for (uint16_t k = from; k < to; ++k) p += re_[k] * re_[k] + im_[k] * im_[k];
    bandPower[b] = p / static_cast<float>((to - from) * w);
  }

  // --- Hüllkurve: Rhythmus der Lautstärke statt Wellenform ---------------------
  // Eine Vibrationspumpe "hämmert" im Netztakt. Unter Druck wird der Klang
  // jedes Schlags rauschiger und ändert sich, der Rhythmus der Schläge bleibt
  // aber exakt gleich. Deshalb wird die Autokorrelation der Hüllkurve
  // (gleichgerichtetes, tiefpassgefiltertes Signal) berechnet.
  // Die Höhenanhebung (Pre-Emphasis) vorab unterdrückt tieffrequentes Rumpeln.
  float prev = 0.0f, env = 0.0f, envSum = 0.0f;
  for (size_t i = 0; i < w; ++i) {
    const float v = history_[i] - dc;
    const float hp = v - kPreEmphasis * prev;
    prev = v;
    env += kEnvelopeAlpha * (fabsf(hp) - env);  // Tiefpass ~500 Hz
    re_[i] = env;
    envSum += env;
  }
  const float envMean = envSum / w;
  for (size_t i = 0; i < w; ++i) {
    re_[i] -= envMean;  // nur die Schwankung der Hüllkurve zählt
    im_[i] = 0.0f;
  }

  // --- Autokorrelation über FFT: ACF = IFFT(|FFT(x)|²) ------------------------
  // Das Auffüllen mit Nullen auf die doppelte Länge verhindert zyklische Effekte.
  for (size_t i = w; i < cfg::kAcfFftSize; ++i) re_[i] = im_[i] = 0.0f;
  fft(cfg::kAcfFftSize);
  for (size_t k = 0; k < cfg::kAcfFftSize; ++k) {
    re_[k] = re_[k] * re_[k] + im_[k] * im_[k];
    im_[k] = 0.0f;
  }
  // Das Leistungsspektrum ist reell und symmetrisch -> eine Vorwärts-FFT
  // liefert (bis auf einen Faktor) dieselbe Autokorrelation wie die inverse.
  fft(cfg::kAcfFftSize);
  // Normierung auf r(0), Korrektur der mit k abnehmenden Überlappung und
  // zeitliche Glättung wie bei den Bändern (beim ersten Frame ungeglättet).
  const float r0 = re_[0];
  const float a = primed_ ? cfg::kSmoothing : 1.0f;
  for (size_t k = 0; k <= cfg::kMaxLag; ++k) {
    const float r = r0 > kEps ? (re_[k] / r0) * (static_cast<float>(w) / (w - k)) : 0.0f;
    acf_[k] = a * r + (1.0f - a) * acf_[k];
  }

  // --- Zeitliche Glättung (linear, dann in dB) ---------------------------------
  if (!primed_) {
    memcpy(smoothBand_, bandPower, sizeof(bandPower));
    smoothPower_ = power;
    primed_ = true;
  } else {
    const float ab = cfg::kBandSmoothing;
    for (size_t b = 0; b < cfg::kNumBands; ++b) {
      smoothBand_[b] = ab * bandPower[b] + (1.0f - ab) * smoothBand_[b];
    }
    const float a = cfg::kSmoothing;
    smoothPower_ = a * power + (1.0f - a) * smoothPower_;
  }
  for (size_t b = 0; b < cfg::kNumBands; ++b) {
    out.bandDb[b] = 10.0f * log10f(smoothBand_[b] + kEps);
  }
  out.rmsDb = 10.0f * log10f(smoothPower_ + 1e-18f);
}

void FeatureExtractor::fft(size_t n) {
  // Bit-Reversal-Permutation
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re_[i]; re_[i] = re_[j]; re_[j] = t;
      t = im_[i]; im_[i] = im_[j]; im_[j] = t;
    }
  }

  // Butterfly-Stufen
  for (size_t len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * kPi / len;
    const float wRe = cosf(ang);
    const float wIm = sinf(ang);
    for (size_t i = 0; i < n; i += len) {
      float curRe = 1.0f, curIm = 0.0f;
      for (size_t k = 0; k < len / 2; ++k) {
        const size_t a = i + k;
        const size_t b = a + len / 2;
        const float tRe = re_[b] * curRe - im_[b] * curIm;
        const float tIm = re_[b] * curIm + im_[b] * curRe;
        re_[b] = re_[a] - tRe;
        im_[b] = im_[a] - tIm;
        re_[a] += tRe;
        im_[a] += tIm;
        const float nRe = curRe * wRe - curIm * wIm;
        curIm = curRe * wIm + curIm * wRe;
        curRe = nRe;
      }
    }
  }
}
