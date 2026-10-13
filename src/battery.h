/**
 * @file battery.h
 * @brief Akkustand des CoreS3 (über den Power-Management-Chip, M5.Power).
 *
 * Der Wert wird zyklisch zwischengespeichert, damit Display und Webseite ihn
 * jederzeit ohne zusätzliche I2C-Zugriffe abfragen können.
 */
#pragma once

#include <stdint.h>

namespace battery {

/// Zyklisch aus loop() aufrufen; liest den Akku alle paar Sekunden aus.
void loop();

/// Ladestand in Prozent (0..100), -1 = unbekannt / kein Akku.
int level();

/// true, solange der Akku geladen wird (USB angeschlossen).
bool charging();

}  // namespace battery
