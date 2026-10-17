// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file datetime.h
 * @brief Uhrzeit und Datum mit der eingebauten RTC (BM8563) des CoreS3.
 *
 * - Beim Start übernimmt M5.begin() die Zeit aus der RTC (UTC) als Systemzeit,
 *   die Uhr läuft damit auch ohne WLAN.
 * - Nach jedem NTP-Abgleich (net_manager) wird die RTC neu gestellt.
 */
#pragma once

#include <stddef.h>

namespace datetime {

/// Nach M5.begin() aufrufen: Zeitzone setzen, NTP-Rückmeldung registrieren.
void begin();

/// Zyklisch aus loop() aufrufen; schreibt die Zeit nach einem NTP-Abgleich in die RTC.
void loop();

/// true, wenn die Systemzeit gestellt ist (aus RTC oder NTP).
bool valid();

/// Lokale Zeit als "16.10.2026 14:32"; leer, wenn die Zeit nicht gestellt ist.
void format(char* buf, size_t size);

}  // namespace datetime
