/**
 * @file scale.h
 * @brief Optionale Anbindung der Felicita-Arc-Kaffeewaage über Bluetooth LE.
 *
 * Protokoll (aus Open-Source-Projekten wie Beanconqueror bekannt):
 *  - Gerätename beginnt mit "FELICITA", Service 0xFFE0, Characteristic 0xFFE1
 *  - Die Waage sendet ~10x/s eine Notification mit 18 Byte:
 *      [2]     '+' oder '-'
 *      [3..8]  Gewicht als 6 ASCII-Ziffern in 0,01 g
 *      [15]    Akku (Rohwert 129..158 entspricht 0..100 %)
 *  - Befehle: einzelnes Byte auf 0xFFE1 schreiben
 *      'T' (0x54) Tara, 'R' (0x52) Timer Start, 'S' (0x53) Timer Stop, 'C' (0x43) Timer Reset
 *
 * Ablauf: Ein eigener Task sucht die Waage, verbindet sich, abonniert die
 * Gewichtsdaten und verbindet sich bei Abbruch automatisch neu. Befehle aus
 * anderen Tasks (Tara usw.) werden nur vorgemerkt und vom Waagen-Task
 * gesendet – alle Funktionen hier sind threadsicher.
 *
 * Ist die Erweiterung ausgeschaltet, wird Bluetooth gar nicht gestartet
 * (spart Speicher).
 */
#pragma once

#include <Arduino.h>

namespace scale {

/// Einstellungen der Waagen-Erweiterung (im NVS gespeichert).
struct Config {
  bool enabled = false;    ///< Erweiterung aktiv (Bluetooth an)
  bool autoTare = true;    ///< beim Pumpenstart automatisch tarieren
  bool syncTimer = false;  ///< Timer der Waage mit der Pumpe starten/stoppen
};

enum class State : uint8_t { Off, Scanning, Connecting, Connected };

/// Lädt die Einstellungen und startet den Waagen-Task.
void begin();

Config config();
/// Übernimmt und speichert neue Einstellungen.
void setConfig(const Config& c);

State state();
const char* stateText();   ///< Klartext für Anzeige
bool connected();

/// Aktuelles Gewicht in g; NAN, wenn keine (aktuellen) Daten vorliegen.
float weight();
/// Durchfluss in g/s (über ~1 s gemittelt); 0 ohne Daten.
float flowRate();
/// Akku der Waage in Prozent, -1 = unbekannt.
int battery();
/// Name der verbundenen Waage.
String deviceName();

// Befehle (werden vom Waagen-Task gesendet)
void tare();
void timerStart();
void timerStop();
void timerReset();

/// Diagnose: empfangene / ungültige Pakete und letztes Rohpaket (hex).
void diagnostics(String& out);

}  // namespace scale
