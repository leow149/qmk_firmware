// SPDX-License-Identifier: GPL-2.0-or-later
// Stock-compatible layout and Fn layer. The stock firmware's tables (decoded from the stock image, see NOTES.md) and the
// F75 manual give: Fn+Q/W/E = Android/Windows/Mac layout, Fn+1/2/3 = Bluetooth slots, Fn+` (hold) = pairing,
// Fn+Win = Windows-key lock, Fn+Tab = lighting colour, Fn+arrows = lighting brightness/speed, Fn+V = lighting effect,
// Fn+F5/F6 = lighting brightness, Fn+F7..F12 = prev / play / next / mute / vol- / vol+, Fn+F1/F2 = browser home / mail.
#include QMK_KEYBOARD_H

enum layers { WIN, FN };

#define ___ KC_NO
#define FN_ MO(FN)

// Matrix positions verified with the probe build (see NOTES.md).
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [WIN] = {
        /*0*/ { KC_ESC,  KC_F1,  KC_F2,  KC_F3,  KC_F4,  KC_F5,  KC_F6,  KC_F7,  KC_F8,  KC_F9,   KC_F10,  KC_F11,  KC_F12,  ___,     KC_MUTE },
        /*1*/ { KC_GRV,  KC_1,   KC_2,   KC_3,   KC_4,   KC_5,   KC_6,   KC_7,   KC_8,   KC_9,    KC_0,    KC_MINS, KC_EQL,  KC_BSPC, KC_DEL  },
        /*2*/ { KC_TAB,  KC_Q,   KC_W,   KC_E,   KC_R,   KC_T,   KC_Y,   KC_U,   KC_I,   KC_O,    KC_P,    KC_LBRC, KC_RBRC, KC_BSLS, KC_PGUP },
        /*3*/ { KC_CAPS, KC_A,   KC_S,   KC_D,   KC_F,   KC_G,   KC_H,   KC_J,   KC_K,   KC_L,    KC_SCLN, KC_QUOT, ___,     KC_ENT,  KC_PGDN },
        /*4*/ { KC_LSFT, ___,    KC_Z,   KC_X,   KC_C,   KC_V,   KC_B,   KC_N,   KC_M,   KC_COMM, KC_DOT,  KC_SLSH, KC_RSFT, KC_UP,   KC_END  },
        /*5*/ { KC_LCTL, KC_LGUI,KC_LALT,___,    ___,    ___,    KC_SPC, ___,    ___,    ___,     KC_RALT, FN_,     KC_LEFT, KC_DOWN, KC_RGHT },
    },
    [FN] = {
        /*0*/ { QK_BOOT, KC_WHOM,KC_MAIL,_______,_______,RM_VALD,RM_VALU,KC_MPRV,KC_MPLY,KC_MNXT, KC_MUTE, KC_VOLD, KC_VOLU, ___,     PG_TOG  },
        /*1*/ { BT_PAIR, BT1,    BT2,    BT3,    _______,_______,_______,_______,_______,_______, _______, _______, _______, _______, KC_INS  },
        /*2*/ { RM_HUEU, OS_AND, OS_WIN, OS_MAC, BT24G,  _______,_______,KC_PSCR,KC_SCRL,KC_PAUS, _______, _______, _______, _______, _______ },
        /*3*/ { _______, _______,_______,_______,_______,_______,_______,_______,_______,_______, _______, _______, ___,     _______, _______ },
        /*4*/ { _______, ___,    _______,_______,RM_PREV,RM_NEXT,RM_TOGG,_______,_______,_______, _______, _______, _______, RM_VALU, KC_HOME },
        /*5*/ { _______, GU_TOGG,_______,___,    ___,    ___,    _______,___,    ___,    ___,     _______, _______, RM_SPDD, RM_VALD, RM_SPDU },
    },
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [WIN] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU) },
    [FN]  = { ENCODER_CCW_CW(RM_VALD, RM_VALU) },
};
#endif

#ifdef TAP_DANCE_ENABLE
// Example tap-dance actions. They do nothing until you assign TD(0) / TD(1) to a key (e.g. in VIA).
enum { TD_ESC_GRV, TD_CTL_CAPS };
tap_dance_action_t tap_dance_actions[] = {
    [TD_ESC_GRV]  = ACTION_TAP_DANCE_DOUBLE(KC_ESC, KC_GRV),    // tap: Esc, double tap: `
    [TD_CTL_CAPS] = ACTION_TAP_DANCE_DOUBLE(KC_LCTL, KC_CAPS),  // tap: Ctrl, double tap: Caps Lock
};
#endif
