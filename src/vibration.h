/**
 * @file vibration.h
 * @brief Pumpenerkennung über Vibration (Beschleunigungssensor BMI270 des CoreS3).
 *
 * Steht der CoreS3 direkt auf der Maschine (z. B. Tassenablage), überträgt
 * die Pumpe ihre Vibration auf das Gehäuse. Das ist unabhängig von
 * Raumgeräuschen und der Mühle, die meist nicht auf der Maschine steht.
 *
 * Messung: Beschleunigung mit 400 Hz über den FIFO des Sensors (erfasst den
 * 50/100-Hz-Takt einer Vibrationspumpe und das Brummen einer Rotationspumpe),
 * Hochpass gegen Schwerkraft/Neigung, Energie je 100-ms-Block in dB.
 * "Aktiv", wenn der Pegel deutlich über dem automatisch nachgeführten
 * Ruhepegel liegt und gleichmäßig ist (Stöße wie Tasse abstellen sind kurz und unruhig).
 *
 * Gemessen wird immer (auch im Modus Mic), verwendet nur in Vib/Both.
 *
 * Betriebsarten (im NVS gespeichert, gelten für alle Profile):
 *  - Mic:   nur Mikrofon (bisheriges Verhalten)
 *  - Vib:   nur Vibration
 *  - Both:  Mikrofon UND Vibration müssen die Pumpe erkennen (am robustesten)
 */
#pragma once

#include <stdint.h>

namespace vibration {

enum class Mode : uint8_t { Mic = 0, Vib = 1, Both = 2 };

struct Config {
  Mode mode = Mode::Mic;
  float thresholdDb = 8.0f;  ///< so viel über dem Ruhepegel gilt als Vibration
};

/// Momentaufnahme (threadsicher kopiert).
struct Status {
  bool available = false;   ///< Sensor gefunden und eingerichtet
  bool active = false;      ///< Vibration erkannt (unentprellt, Entprellung im Detektor)
  uint32_t sinceMs = 0;     ///< Beginn der aktuellen lauten Phase (millis)
  float levelDb = -120.0f;  ///< aktueller Vibrationspegel (dB re 1 g²)
  float baselineDb = -120.0f;  ///< Ruhepegel
  float steadiness = 99.0f; ///< Pegelschwankung der lauten Phase in dB
};

/// Nach M5.begin(): Sensor auf 400 Hz / FIFO umstellen, Einstellungen laden.
void begin();

/// Zyklisch aus loop() aufrufen (selber Task wie Touch/Akku -> kein I2C-Konflikt).
void loop();

Config config();
void setConfig(const Config& c);

Status status();

const char* modeName(Mode m);

}  // namespace vibration
