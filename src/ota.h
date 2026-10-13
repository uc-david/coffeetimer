/**
 * @file ota.h
 * @brief Firmware-Update über WLAN (Over-The-Air).
 *
 * Zwei Wege:
 *  - ArduinoOTA: direkt aus PlatformIO flashen
 *      pio run -e m5stack-cores3-ota -t upload
 *    (Gerät muss im selben Netz unter coffeetimer.local erreichbar sein)
 *  - Webseite: .bin-Datei unter "Firmware-Update" hochladen
 *    (siehe web_server.cpp, Route /api/update)
 *
 * Die Partitionstabelle (default_16MB.csv) hat zwei App-Slots: Das Update wird
 * in den freien Slot geschrieben und erst nach erfolgreicher Prüfung aktiviert.
 * Bricht die Übertragung ab, startet weiter die alte Firmware.
 *
 * Optional kann ein Passwort für ArduinoOTA gesetzt werden:
 *   build_flags = -DOTA_PASSWORD=\"geheim\"
 * und in der OTA-Umgebung: upload_flags = --auth=geheim
 */
#pragma once

namespace ota {

/// Startet den ArduinoOTA-Dienst. Erst aufrufen, wenn WLAN-Modus gesetzt ist.
void begin();

/// Zyklisch aus loop() aufrufen.
void loop();

}  // namespace ota
