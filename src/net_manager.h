/**
 * @file net_manager.h
 * @brief WLAN-Verwaltung mit Access-Point-Fallback, mDNS und NTP.
 *
 * - Sind WLAN-Zugangsdaten gespeichert, wird eine Verbindung versucht.
 * - Gelingt das nicht (oder ist nichts konfiguriert), öffnet das Gerät einen
 *   eigenen Access-Point "CoffeeTimer-XXXX" (offen, IP 192.168.4.1). Über die
 *   Webseite können dort die WLAN-Daten eingetragen werden.
 * - Im Heimnetz ist das Gerät zusätzlich unter http://coffeetimer.local erreichbar.
 */
#pragma once

#include <Arduino.h>

#include "settings.h"

namespace net {

/// Startet den Verbindungsaufbau (nicht blockierend).
void begin(const WifiSettings& wifi);

/// Zyklisch aus loop() aufrufen: Zustandsüberwachung, AP-Fallback.
void loop();

/// true, wenn mit dem Heim-WLAN verbunden.
bool connected();

/// true, wenn der eigene Access-Point aktiv ist.
bool apActive();

/// true, solange die Verbindung zum Heim-WLAN aufgebaut wird.
bool connecting();

/// Aktuelle IP-Adresse (Heimnetz oder AP) als Text.
String ip();

/// Name des verbundenen WLANs bzw. des eigenen Access-Points.
String networkName();

}  // namespace net
