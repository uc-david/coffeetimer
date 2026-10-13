/**
 * @file battery.cpp
 * @brief Implementierung der Akkuabfrage.
 */
#include "battery.h"

#include <M5Unified.h>

namespace {

constexpr uint32_t kIntervalMs = 2000;  ///< Abfrageintervall

int cachedLevel = -1;
bool cachedCharging = false;
uint32_t lastReadMs = 0;
bool firstRead = true;

}  // namespace

namespace battery {

void loop() {
  const uint32_t now = millis();
  if (!firstRead && now - lastReadMs < kIntervalMs) return;
  firstRead = false;
  lastReadMs = now;

  const int32_t lvl = M5.Power.getBatteryLevel();
  cachedLevel = (lvl >= 0 && lvl <= 100) ? lvl : -1;
  cachedCharging = M5.Power.isCharging() == m5::Power_Class::is_charging;
}

int level() { return cachedLevel; }

bool charging() { return cachedCharging; }

}  // namespace battery
