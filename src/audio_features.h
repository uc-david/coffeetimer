// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file audio_features.h
 * @brief Merkmalsextraktion aus dem Audiosignal (Pegel, Bandspektrum, Takt).
 *
 * Alle 32 ms (ein Aufnahme-Block) wird ein 64 ms langes Fenster (die letzten
 * zwei Blöcke) ausgewertet:
 *  - Gesamtpegel in dBFS
 *  - Energie in cfg::kNumBands logarithmisch verteilten Frequenzbändern (dB) –
 *    der "akustische Fingerabdruck" (Klangmuster)
 *  - Autokorrelation der Hüllkurve: Wie stark wiederholt sich der
 *    Lautstärkeverlauf nach einer bestimmten Zeit? Vibrationspumpen schlagen
 *    im Netztakt (50 Hz = 20 ms);
 *    dieser Takt bleibt auch unter Druck gleich, während sich das
 *    Klangmuster verändert. Rotationspumpen laufen gleichmäßig und haben
 *    meist keinen ausgeprägten Takt – dort zählt vor allem das Klangmuster.
 *
 * Pegel und Bänder werden zusätzlich zeitlich geglättet, damit das
 * Pulsieren einer Vibrationspumpe nicht als Schwankung erscheint.
 */
#pragma once

#include <stdint.h>

#include "config.h"

/// Ergebnis der Analyse eines Frames.
struct AudioFrame {
  float bandDb[cfg::kNumBands];  ///< Bandenergien in dB (geglättet, relativ)
  float rmsDb;                   ///< Gesamtpegel in dBFS (geglättet)
  float clipRatio;               ///< Anteil übersteuerter Samples im neuen Block (0..1)
  uint32_t timestampMs;          ///< millis() zum Zeitpunkt der Auswertung
};

/// Berechnet AudioFrame-Merkmale aus Roh-Samples mittels FFT.
class FeatureExtractor {
 public:
  /// Berechnet Fensterfunktion und Bandgrenzen. Einmal beim Start aufrufen.
  void begin();

  /**
   * Analysiert den nächsten Aufnahme-Block.
   * @param samples   Mono-PCM-Daten, genau cfg::kFrameSize Werte
   * @param out       Ergebnis
   */
  void process(const int16_t* samples, AudioFrame& out);

  /**
   * "Rhythmus-Kurve": geglättete, normierte Autokorrelation der Hüllkurve für
   * die Verschiebungen cfg::kMinLag .. cfg::kMaxLag (cfg::kAcfLen Werte).
   * 1.0 = Lautstärkeverlauf wiederholt sich nach dieser Zeit exakt.
   * Die Form der Kurve ist ein Fingerabdruck des Rhythmus: Eine
   * Vibrationspumpe (ein Schlag je Netzperiode) hat Spitzen bei 20/40 ms und
   * Täler dazwischen, ein brummender Motor (100 Hz) zusätzlich bei 10/30 ms.
   */
  const float* acfCurve() const { return acf_ + cfg::kMinLag; }

 private:
  /// In-place Radix-2-FFT (Cooley-Tukey) der ersten n Werte von re_/im_.
  void fft(size_t n);

  float history_[cfg::kWindowSize];         ///< die letzten kWindowSize Samples
  float re_[cfg::kAcfFftSize];
  float im_[cfg::kAcfFftSize];
  float window_[cfg::kWindowSize];          ///< Hann-Fenster für das Spektrum
  float acf_[cfg::kMaxLag + 1];             ///< normierte Autokorrelation (geglättet)
  uint16_t bandStart_[cfg::kNumBands + 1];  ///< FFT-Bin-Grenzen der Bänder
  float smoothBand_[cfg::kNumBands];        ///< geglättete Bandleistung (linear)
  float smoothPower_ = 0.0f;                ///< geglättete Gesamtleistung (linear)
  bool primed_ = false;                     ///< Glättung initialisiert?
};
