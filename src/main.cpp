// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file main.cpp
 * @brief Coffee Timer – Einstiegspunkt.
 *
 * Erkennt am Geräusch, wann die Pumpe einer Siebträger-Kaffeemaschine läuft,
 * und misst die Bezugszeit. Anzeige auf dem M5Stack CoreS3 und parallel über
 * eine Webseite (http://coffeetimer.local bzw. IP-Adresse am Display).
 *
 * Modulübersicht:
 *   audio_features  FFT-Merkmale je Audio-Frame (Pegel, Bandspektrum)
 *   classifier      KI-Erkennung: auf dem Gerät trainierter Klassifikator
 *   pump_detector   Pegel, Gleichmäßigkeit, KI-Entscheidung, Ein-/Aus-Entprellung
 *   teach_mode      Einlernen von Pumpe, Mühlen, Störgeräuschen inkl. Qualitätsbewertung
 *   shot_timer      Bezugs-Timer und Historie
 *   app             zentrale Logik, Audio-Task, Threadsicherheit
 *   ui              Display und Touch
 *   web_server      Webseite und JSON-API
 *   net_manager     WLAN / Access-Point / mDNS / NTP
 *   ota             Firmware-Update über WLAN (PlatformIO / Webseite)
 *   battery         Akkustand für Display und Webseite
 *   vibration       Pumpenerkennung über den Beschleunigungssensor (optional)
 *   datetime        Uhrzeit/Datum, eingebaute RTC (per NTP gestellt)
 *   scale           optionale Bluetooth-Waage (Felicita, Acaia, Bookoo, Decent)
 *   settings        Persistenz (Profile im LittleFS, kleine Werte im NVS)
 */
#include <M5Unified.h>

#include "app.h"
#include "battery.h"
#include "datetime.h"
#include "net_manager.h"
#include "ota.h"
#include "scale.h"
#include "settings.h"
#include "ui.h"
#include "vibration.h"
#include "web_server.h"

void setup() {
  auto m5cfg = M5.config();
  m5cfg.serial_baudrate = 115200;
  m5cfg.internal_mic = true;
  m5cfg.internal_spk = false;  // Lautsprecher wird nicht benötigt (teilt I2S mit Mikrofon)
  M5.begin(m5cfg);

  Serial.println("\n=== Coffee Timer ===");

  datetime::begin();  // Zeitzone, RTC-Zeit wurde von M5.begin() übernommen
  settings::begin();
  ui::begin();
  app().begin();
  vibration::begin();  // Beschleunigungssensor (nur aktiv, wenn als Quelle gewählt)
  scale::begin();  // optionale Waage (Bluetooth nur, wenn eingeschaltet)

  WifiSettings wifi;
  settings::loadWifi(wifi);
  net::begin(wifi);
  web::begin();
  ota::begin();
}

void loop() {
  M5.update();  // Touch einlesen
  battery::loop();
  vibration::loop();
  datetime::loop();
  app().loop();  // verzögertes Speichern der Einstellungen
  ui::loop();
  net::loop();
  web::loop();
  ota::loop();
  delay(5);
}
