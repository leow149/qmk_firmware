// SPDX-License-Identifier: GPL-2.0-or-later
// Status helpers. The UART link to the wireless module (battery poll, BT/2.4G transport) lives in the module driver
// bluetooth/ch582f_ajazz.c (same protocol as the AK820 Pro); here we only add the charger-IC pins.
#include QMK_KEYBOARD_H
#include "module.h"
#include "bluetooth/ch582f_ajazz.h"
#include "gpio.h"

#define PIN_CHRG  B16                // open-drain, LOW while charging
#define PIN_STDBY B17                // open-drain, LOW when charge is complete

void module_init(void) {
    gpio_set_pin_input_high(PIN_CHRG);
    gpio_set_pin_input_high(PIN_STDBY);
}
void module_task(void) {}

uint8_t module_battery(void) { return ch582_get_battery(); }   // 0xFF until the module answers (== BATT_UNKNOWN)

charge_state_t module_charge_state(void) {
    bool chrg_low  = !gpio_read_pin(PIN_CHRG);
    bool stdby_low = !gpio_read_pin(PIN_STDBY);
    if (chrg_low && !stdby_low) return CHG_CHARGING;
    if (stdby_low)              return CHG_FULL;
    return CHG_NONE;
}
