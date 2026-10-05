// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "quantum.h"

enum f75max_keycodes {
    BT1 = QK_KB_0,      // Fn+1: BT slot 1 (tap select, hold pair) -- BT mode
    BT2,                // Fn+2
    BT3,                // Fn+3
    BT24G,              // Fn+R: 2.4G (2.4G mode)
    BT_PAIR,            // Fn+` long press: pair
    OS_AND,             // Fn+Q: Android layout
    OS_WIN,             // Fn+W: Windows layout
    OS_MAC,             // Fn+E: Mac layout
    F75MAX_SAFE_RANGE
};

typedef enum { MODE_USB = 0, MODE_BT = 1, MODE_24G = 2 } conn_mode_t;
void        f75max_apply_mode(conn_mode_t m);   // called when the mode slider changes (and once at boot)
conn_mode_t f75max_conn_mode(void);
void        f75max_load_config(void);        // restore the persisted Bluetooth slot (call before f75max_apply_mode)
void        f75max_recovery_task(void);      // hard-wired Esc+Backspace (3 s) -> bootloader, independent of the keymap
void        display_bootloader_notice(void); // shows "BOOTLOADER" on the LCD just before the jump
