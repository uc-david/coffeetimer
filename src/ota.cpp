// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file ota.cpp
 * @brief ArduinoOTA-Anbindung mit Fortschrittsanzeige am Display.
 */
#include "ota.h"

#include <ArduinoOTA.h>

#include "config.h"
#include "ui.h"

namespace ota {

void begin() {
  ArduinoOTA.setHostname(cfg::kHostname);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  // mDNS wird bereits von net_manager gestartet.
  ArduinoOTA.setMdnsEnabled(false);

  ArduinoOTA.onStart([] {
    Serial.println("[ota] Update gestartet");
    ui::showUpdateProgress(0, "Firmware-Update (OTA)");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    ui::showUpdateProgress(total ? done * 100 / total : 0, "Firmware-Update (OTA)");
  });
  ArduinoOTA.onEnd([] {
    Serial.println("[ota] Update fertig – Neustart");
    ui::showUpdateProgress(100, "Update fertig – Neustart …");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("[ota] Fehler %u\n", err);
    ui::showUpdateProgress(-1, "Update fehlgeschlagen");
  });
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle();
}

}  // namespace ota
