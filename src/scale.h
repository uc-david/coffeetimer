/**
 * @file scale.h
 * @brief Optionale Anbindung einer Bluetooth-Kaffeewaage (BLE).
 *
 * Unterstützte Waagen (Protokolle aus Open-Source-Projekten wie Beanconqueror,
 * AcaiaArduinoBLE und der offiziellen Decent-API):
 *  - Felicita Arc / Incline / Parallel  (Service 0xFFE0, ASCII-Protokoll)
 *  - Acaia Lunar, Pearl, Pyxis, Cinco, Orion … (alte und neue Firmware)
 *  - Bookoo Themis Mini / Ultra           (0xFF11 / 0xFF12)
 *  - Decent Scale / Half Decent Scale     (0xFFF4 / 0x36F5)
 * Gefunden wird jede Waage, deren Name passt; das Protokoll wird nach dem
 * Verbinden an den vorhandenen Characteristics erkannt.
 *
 * Ablauf: Ein eigener Task sucht die Waage, verbindet sich, abonniert die
 * Gewichtsdaten und verbindet sich bei Abbruch automatisch neu. Befehle aus
 * anderen Tasks (Tara usw.) werden nur vorgemerkt und vom Waagen-Task
 * gesendet – alle Funktionen hier sind threadsicher.
 *
 * Dosier-Tara: Beim Mahlen wird die (leere) Waage tariert und das Gewicht des
 * Siebträgers abgezogen – die Anzeige startet bei z. B. -520 g und zeigt nach
 * dem Auflegen des gefüllten Siebträgers direkt das Kaffeemehl.
 *
 * Ist die Erweiterung ausgeschaltet, wird Bluetooth gar nicht gestartet
 * (spart Speicher).
 */
#pragma once

#include <Arduino.h>

namespace scale {

/// Einstellungen der Waagen-Erweiterung (im NVS gespeichert).
struct Config {
  bool enabled = false;      ///< Erweiterung aktiv (Bluetooth an)
  bool autoTare = true;      ///< beim Pumpenstart automatisch tarieren
  bool syncTimer = false;    ///< Timer der Waage mit der Pumpe starten/stoppen
  bool grindTare = false;    ///< beim Mahlen auf -Siebträgergewicht tarieren
  float portafilterG = 0.0f; ///< Gewicht des leeren Siebträgers in g
};

enum class State : uint8_t { Off, Scanning, Connecting, Connected };

/// Lädt die Einstellungen und startet den Waagen-Task.
void begin();

Config config();
/// Übernimmt und speichert neue Einstellungen.
void setConfig(const Config& c);

const char* stateText();   ///< Klartext für Anzeige
bool connected();

/// Aktuelles Gewicht in g (inkl. Dosier-Versatz); NAN, wenn keine (aktuellen) Daten vorliegen.
float weight();
/// Gewicht ist seit ~1 s ruhig (z. B. Siebträger abgelegt).
bool stable();
/// Durchfluss in g/s (über ~1 s gemittelt); 0 ohne Daten.
float flowRate();
/// Akku der Waage in Prozent, -1 = unbekannt.
int battery();
/// Name der verbundenen Waage.
String deviceName();
/// Erkanntes Protokoll der verbundenen Waage ("Acaia", "Bookoo" …), leer = keine.
const char* model();

// Befehle (werden vom Waagen-Task gesendet)
/// Tariert auf 0 (beendet die Dosier-Tara).
void tare();
/// Tariert und zieht das Siebträgergewicht ab (Anzeige startet negativ).
void tareForDose();
/// Beendet die Dosier-Tara ohne zu tarieren.
void clearDoseOffset();
/// true, solange die Dosier-Tara aktiv ist.
bool doseMode();
/// Zählt jede angeforderte Tara (tare/tareForDose) – zum Erkennen, dass das
/// Gewicht seit einem Zeitpunkt auf 0 gesetzt wurde.
uint32_t tareCount();
/// Übernimmt das aktuelle Gewicht als Siebträgergewicht. false = keine plausiblen Daten.
bool learnPortafilter();

void timerStart();
void timerStop();
void timerReset();

/// Diagnose: empfangene / ungültige Pakete und letztes Rohpaket (hex).
void diagnostics(String& out);

}  // namespace scale
