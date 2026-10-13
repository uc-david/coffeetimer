/**
 * @file ui.h
 * @brief Bedienoberfläche auf dem CoreS3-Display (320x240, Touch).
 *
 * Screens:
 *  - Main:  großer Timer, Status, Live-Messwerte, Buttons TEACH/RESET/INFO
 *  - TeachSelect: Auswahl "Pumpe" oder "Störgeräusch" (z. B. Mühle) einlernen
 *  - Teach: Anleitung, Fortschritt, Mikrofonpegel; danach Ergebnis mit Begründung
 *           und Buttons SPEICHERN/WIEDERHOLEN/VERWERFEN
 *  - Info:  Netzwerk, Profil, Einstellungen; Empfindlichkeit per +/- anpassbar
 *
 * Gezeichnet wird in einen Sprite im PSRAM und dann komplett übertragen –
 * dadurch flackerfrei.
 */
#pragma once

namespace ui {

/// Initialisiert Display und Zeichenpuffer.
void begin();

/// Zyklisch aus loop() aufrufen (nach M5.update()): Touch auswerten, neu zeichnen.
void loop();

/**
 * Zeigt den Fortschritt eines Firmware-Updates sofort an (blockierende
 * Update-Routinen rufen loop() währenddessen nicht auf).
 * @param percent  0..100, negativ = Fehler
 * @param title    Überschrift
 */
void showUpdateProgress(int percent, const char* title);

}  // namespace ui
