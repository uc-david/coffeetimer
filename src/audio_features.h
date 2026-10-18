// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file audio_features.h
 * @brief Merkmalsextraktion aus dem Audiosignal (Pegel, Bandspektrum).
 *
 * Alle 32 ms (ein Aufnahme-Block) wird ein 64 ms langes Fenster (die letzten
 * zwei Blöcke) ausgewertet:
 *  - Gesamtpegel in dBFS
 *  - Energie in cfg::kNumBands logarithmisch verteilten Frequenzbändern (dB) –
 *    daraus bildet die KI-Erkennung die Klangfarbe (classifier::quantizeShape)
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

 private:
  /// In-place Radix-2-FFT (Cooley-Tukey) der ersten n Werte von re_/im_.
  void fft(size_t n);

  float history_[cfg::kWindowSize];         ///< die letzten kWindowSize Samples
  float re_[cfg::kWindowSize];
  float im_[cfg::kWindowSize];
  float window_[cfg::kWindowSize];          ///< Hann-Fenster für das Spektrum
  uint16_t bandStart_[cfg::kNumBands + 1];  ///< FFT-Bin-Grenzen der Bänder
  float smoothBand_[cfg::kNumBands];        ///< geglättete Bandleistung (linear)
  float smoothPower_ = 0.0f;                ///< geglättete Gesamtleistung (linear)
  bool primed_ = false;                     ///< Glättung initialisiert?
};
