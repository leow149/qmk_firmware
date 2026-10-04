// SPDX-License-Identifier: GPL-2.0-or-later
#include "quantum.h"
#include "gpio.h"

// The 18 LED row drivers (NPN, active high), R/B/G x 6 rows, shared with the AK820 Pro design.
// Keep them LOW until RGB support exists, so no LED lights while the shared columns are strobed.
static const pin_t led_row_pins[] = { A11, B4, B5, A8, A9, D8, D9, D10, D11, D12, D13, D16, D17, D18, C10, C11, C12, C13 };

void keyboard_pre_init_kb(void) {
    for (uint8_t i = 0; i < ARRAY_SIZE(led_row_pins); i++) {
        gpio_set_pin_output(led_row_pins[i]);
        gpio_write_pin_low(led_row_pins[i]);
    }
    keyboard_pre_init_user();
}

// Route SPI0 to P3.0 (SCK) / P3.2 (MOSI) / P3.1 (MISO unused) / SEL0 -- same PFPA values the stock firmware writes.
void early_hardware_init_post(void) {
    SN_PFPA->SPI_b.MISO0 = 0b11;
    SN_PFPA->SPI_b.MOSI0 = 0b11;
    SN_PFPA->SPI_b.SCK0  = 0b11;
    SN_PFPA->SPI_b.SEL0  = 0b10;
    // UART2 to the wireless module on P1.7 (TX) / P1.6 (RX), same PFPA values as the stock firmware.
    SN_PFPA->UART_b.UTXD2 = 0b11;
    SN_PFPA->UART_b.URXD2 = 0b11;
}

// ---------------- wireless glue (module driver in bluetooth/ch582f_ajazz.c, same protocol as the AK820 Pro) ----------------
#include "bluetooth/ch582f_ajazz.h"
#include "connection.h"
#include "timer.h"
#include "f75max.h"

#define BT_PAIR_HOLD_MS 1000
static conn_mode_t     wireless_mode   = MODE_USB;
static ch582_profile_t last_bt_profile = CH582_PROFILE_BT_1;
static uint16_t        bt_pair_timer   = 0;
static bool            bt_pair_armed   = false;

conn_mode_t f75max_conn_mode(void) { return wireless_mode; }

typedef struct __attribute__((packed)) { uint8_t bt_profile; uint8_t _pad[3]; } kb_config_t;
static kb_config_t kb_config;

void f75max_load_config(void) {
    eeconfig_read_kb_datablock(&kb_config, 0, sizeof(kb_config));
    if (kb_config.bt_profile >= CH582_PROFILE_BT_1 && kb_config.bt_profile <= CH582_PROFILE_BT_3)
        last_bt_profile = (ch582_profile_t)kb_config.bt_profile;     // otherwise keep the default (slot 1)
    // Restore the saved layout (Windows/Android = layer 0, Mac = layer 1). Ignore anything else so we can never end up with no layer.
    layer_state_t dl = eeconfig_read_default_layer();
    if (dl == (1UL << 0) || dl == (1UL << 1)) default_layer_set(dl);
}
// Write only when the slot actually changed: the EEPROM emulation lives in flash.
static void save_bt_profile(ch582_profile_t p) {
    last_bt_profile = p;
    if (kb_config.bt_profile == (uint8_t)p) return;
    kb_config.bt_profile = (uint8_t)p;
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

void f75max_apply_mode(conn_mode_t m) {
    wireless_mode = m;
    switch (m) {
        case MODE_BT:
            ch582_set_profile(last_bt_profile);                 // resume the slot last selected
            connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
            break;
        case MODE_24G:
            ch582_set_profile(CH582_PROFILE_PEER_24G);
            connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);   // the module carries 2.4G too
            break;
        default:
            ch582_cancel_connect();
            connection_set_host_noeeprom(CONNECTION_HOST_USB);
            break;
    }
}

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (!process_record_user(keycode, record)) return false;
    switch (keycode) {
        // TAP selects the slot (reconnects the existing bond); HOLD (>= 1 s) additionally enters pairing.
        case BT1: case BT2: case BT3:
            if (record->event.pressed) {
                if (wireless_mode == MODE_BT) {
                    save_bt_profile(keycode == BT1 ? CH582_PROFILE_BT_1 : keycode == BT2 ? CH582_PROFILE_BT_2 : CH582_PROFILE_BT_3);
                    ch582_set_profile(last_bt_profile);
                    connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
                    bt_pair_timer = timer_read();
                    bt_pair_armed = true;
                }
            } else if (bt_pair_armed) {
                bt_pair_armed = false;
                if (timer_elapsed(bt_pair_timer) >= BT_PAIR_HOLD_MS) ch582_enter_pairing();
            }
            return false;
        // OS layouts (persisted default layer): Android and Windows share the Windows layout, Mac swaps Alt/Win.
        case OS_AND: case OS_WIN:
            if (record->event.pressed) { eeconfig_update_default_layer(1UL << 0); default_layer_set(1UL << 0); }
            return false;
        case OS_MAC:
            if (record->event.pressed) { eeconfig_update_default_layer(1UL << 1); default_layer_set(1UL << 1); }
            return false;
        case BT24G:
            if (record->event.pressed && wireless_mode == MODE_24G) {
                ch582_set_profile(CH582_PROFILE_PEER_24G);
                connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
            }
            return false;
        case BT_PAIR:
            if (record->event.pressed) {
                bt_pair_armed = (wireless_mode != MODE_USB);
                bt_pair_timer = timer_read();
            } else if (bt_pair_armed) {
                bt_pair_armed = false;
                if (timer_elapsed(bt_pair_timer) >= BT_PAIR_HOLD_MS) ch582_enter_pairing();
            }
            return false;
        default:
            return true;
    }
}

// Windows-key lock indicator: the Win key's LED goes red while the GUI keys are disabled (Fn+Win).
bool rgb_matrix_indicators_kb(void) {
    if (!rgb_matrix_indicators_user()) return false;
    if (keymap_config.no_gui) {
        uint8_t i = g_led_config.matrix_co[5][1];
        if (i != NO_LED) rgb_matrix_set_color(i, 255, 0, 0);
    }
    return true;
}
