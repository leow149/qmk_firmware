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
#ifdef VIA_ENABLE
#    include "via.h"
#endif

#define BT_PAIR_HOLD_MS 1000
static conn_mode_t     wireless_mode   = MODE_USB;
static ch582_profile_t last_bt_profile = CH582_PROFILE_BT_1;
static uint16_t        bt_pair_timer   = 0;
static bool            bt_pair_armed   = false;

conn_mode_t f75max_conn_mode(void) { return wireless_mode; }

#define LAYOUT_VER 2   // 1 = Mac was a separate layer (Fn = layer 2); 2 = Mac is QMK's Alt/Win swap (Fn = layer 1)
typedef struct __attribute__((packed)) { uint8_t bt_profile; uint8_t layout_ver; uint8_t _pad[2]; } kb_config_t;
static kb_config_t kb_config;

void f75max_load_config(void) {
    eeconfig_read_kb_datablock(&kb_config, 0, sizeof(kb_config));
    if (kb_config.bt_profile >= CH582_PROFILE_BT_1 && kb_config.bt_profile <= CH582_PROFILE_BT_3)
        last_bt_profile = (ch582_profile_t)kb_config.bt_profile;     // otherwise keep the default (slot 1)
    // Layouts used to be separate layers (Mac = layer 1, Fn = layer 2). A saved default layer or VIA keymap from that scheme
    // would now point at the wrong layers (Fn as the base layer, Fn key to a layer that does not exist), so reset them once.
    if (kb_config.layout_ver != LAYOUT_VER) {
        eeconfig_update_default_layer(1UL << 0);
        default_layer_set(1UL << 0);
#ifdef VIA_ENABLE
        eeconfig_init_via();
#endif
        kb_config.layout_ver = LAYOUT_VER;
        eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
    }
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
        // OS layouts: Android and Windows share the Windows layout, Mac uses QMK's Alt<->Win swap (saved in EEPROM by QMK).
        case OS_AND: case OS_WIN: case OS_MAC:
            if (record->event.pressed) {
                keymap_config.swap_lalt_lgui = keymap_config.swap_ralt_rgui = (keycode == OS_MAC);
                eeconfig_update_keymap(&keymap_config);
            }
            return false;
        case PG_TOG:
            if (record->event.pressed) display_toggle_page();
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

// ---- recovery chord ----------------------------------------------------------------------------------------
// Hold Esc + Backspace for 3 s to enter the bootloader. It reads the raw key matrix (Esc = [0,0], Backspace = [1,13]),
// so it works whatever the keymap says (VIA can remap Fn+Esc away) and in every connection mode.
#define RECOVERY_HOLD_MS 3000
void f75max_recovery_task(void) {
    static uint32_t since = 0;
    static bool     armed = false;
    bool held = (matrix_get_row(0) & (1u << 0)) && (matrix_get_row(1) & (1u << 13));
    if (!held) { armed = false; return; }
    if (!armed) { armed = true; since = timer_read32(); return; }
    if (timer_elapsed32(since) >= RECOVERY_HOLD_MS) {
        display_bootloader_notice();
        wait_ms(1000);                 // leave the message readable before the jump
        reset_keyboard();
    }
}

// ---- raw HID: PC metrics from the host (see tools/f75max_metrics.py). VIA's own commands are untouched. ----
#ifdef VIA_ENABLE
bool via_command_kb(uint8_t *data, uint8_t length) { return f75max_hid_command(data, length); }
#else
void raw_hid_receive(uint8_t *data, uint8_t length) { f75max_hid_command(data, length); }
#endif
