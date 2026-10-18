// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file web_server.h
 * @brief HTTP-Server: Webseite und JSON-API parallel zur Bedienung am Display.
 *
 * API-Übersicht:
 *   GET  /                  Webseite (Single-Page, siehe web_page.h)
 *   GET  /api/status        kompletter Zustand als JSON (wird ~4x/s abgefragt)
 *   GET  /api/settings      Erkennungseinstellungen
 *   POST /api/settings      Erkennungseinstellungen setzen   (JSON-Body)
 *   POST /api/teach/start   Teach-Modus starten, Body {"mode":"pump"|"grinder"|"noise"}
 *   POST /api/teach/finish  Pumpenaufnahme vorzeitig beenden ("Fertig")
 *   POST /api/teach/cancel  Teach-Modus abbrechen / verwerfen
 *   POST /api/teach/save    Teach-Ergebnis übernehmen, Body {"mode":"replace"|"add"}
 *   POST /api/noise/clear   gelernte Störgeräusche löschen
 *   POST /api/noise/delete  einzelnes Störgeräusch löschen, Body {"index": n}
 *   POST /api/grinders/delete|rename  Mühle löschen / umbenennen, Body {"index": n, "name": "..."}
 *   GET  /api/profiles      Liste der Profil-Slots
 *   POST /api/profiles/select|rename|delete|import   Body {"slot": n, ...}
 *   GET  /api/profiles/export?slot=n   Profil als Datei (JSON, inkl. KI-Trainingsdaten)
 *   GET  /api/profiles/text?slot=n     Profil als Klartext
 *   POST /api/scale         Waage, Body {"enabled","autoTare","syncTimer"} (bool)
 *   POST /api/scale/tare    Waage tarieren
 *   POST /api/mic           Mikrofon-Verstärkung, Body {"gain": 1..64}
 *   GET  /api/diag          Diagnosebericht (Klartext) mit Profil und letzten Erkennungen
 *   POST /api/timer/reset   Timeranzeige auf 0
 *   POST /api/history/clear Historie löschen
 *   POST /api/wifi          WLAN-Daten speichern + Neustart   (JSON-Body)
 *   POST /api/update        Firmware-Upload (multipart, firmware.bin) + Neustart
 */
#pragma once

namespace web {

/// Registriert alle Routen und startet den Server auf Port 80.
void begin();

/// Zyklisch aus loop() aufrufen: bearbeitet eingehende Anfragen.
void loop();

}  // namespace web
