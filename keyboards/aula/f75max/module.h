// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BATT_UNKNOWN 0xFF
typedef enum { CHG_NONE = 0, CHG_CHARGING = 1, CHG_FULL = 2 } charge_state_t;

void           module_init(void);
void           module_task(void);          // call from housekeeping
uint8_t        module_battery(void);       // 0..100, or BATT_UNKNOWN if the module hasn't answered
charge_state_t module_charge_state(void);  // from the charger IC pins (CHRG=B16, STDBY=B17)
