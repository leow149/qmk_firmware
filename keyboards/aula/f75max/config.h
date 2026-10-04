// SPDX-License-Identifier: GPL-2.0-or-later
// AULA F75 Max. Pin sets taken from the verified AK820 Pro port (same SG8975-family PCB design).
#pragma once

/* RGB matrix (hardware PWM on SN32F299). The 18 hardware row pins are 6 key-rows
 * x 3 colour channels in R,B,G order per group. Columns are shared with the key
 * matrix (COL_PINS omitted -> SHARED_MATRIX uses MATRIX_COL_PINS). */
#define SN32F2XX_RGB_MATRIX_ROW_PINS { A11, B4, B5, A8, A9, D8, D9, D10, D11, D12, D13, D16, D17, D18, C10, C11, C12, C13 }
#define SN32F2XX_PWM_CONTROL   HARDWARE_PWM
#define SN32F2XX_PWM_DIRECTION COL2ROW      // hardware PWM on columns, rows are the mux select
#define SN32F2XX_RGB_MATRIX_ROW_CHANNELS 3  // R, B, G

/* 15 columns exceed CT16B1's 12 PWM channels -> three timers. Map is in COL_PINS order:
 *   A4 A5 C0 C1 C2 C3 A6 A7 C4 C5 C6 C7 C14 C8 C9 */
#define SN32F2XX_PWM_MULTI_TIMER
#define SN32F2XX_PWM_COL_MAP { \
    {&PWMD1, 4}, {&PWMD1, 5}, {&PWMD1, 8}, {&PWMD2, 1}, {&PWMD2, 2}, {&PWMD2, 3}, \
    {&PWMD1, 6}, {&PWMD1, 7}, {&PWMD1, 9}, {&PWMD0, 1}, {&PWMD0, 0}, {&PWMD1, 10}, \
    {&PWMD0, 3}, {&PWMD0, 2}, {&PWMD1, 11} }
#define SN32F2XX_PWM_PFPA_CT16B0 0x3332
#define SN32F2XX_PWM_PFPA_CT16B1 0x0F00
#define SN32F2XX_PWM_PFPA_CT16B2 0x0000

// Let rgb_matrix_set_suspend_state() actually blank the matrix.
#define RGB_MATRIX_SLEEP

// Columns are shared between the key matrix and the (column-active-LOW) LED matrix.
// Drive unselected key-rows HIGH so a keypress doesn't light its whole LED column.
#define MATRIX_UNSELECT_DRIVE_HIGH

// Full brightness range: the keyboard has a battery to buffer USB current peaks. Boot at a moderate level.
#define RGB_MATRIX_MAXIMUM_BRIGHTNESS 255
#define RGB_MATRIX_DEFAULT_VAL 100

/* LCD: GC9107-compatible 128x128 panel on SPI0. CS is a GPIO (SS pin), D/C, RESET and backlight are GPIOs. */
#define SPI_DRIVER SPID0
#define SPI_MOSI_PIN D2
#define SPI_SCK_PIN  D0
#define SPI_MISO_PIN NO_PIN
#define SPI_SS_PIN   B8
#define QUANTUM_PAINTER_DISPLAY_TIMEOUT 0
#define QUANTUM_PAINTER_SUPPORTS_NATIVE_COLORS TRUE

#define PANEL_WIDTH  128
#define PANEL_HEIGHT 128
#define PANEL_CS  B8
#define PANEL_DC  D14
#define PANEL_RST A17
#define PANEL_BKL A16     /* ACTIVE HIGH on the F75 Max (stock: low during init, high after first frame) */

/* Boot default; the mode slider selects the real host (f75max_apply_mode). */
#define CONNECTION_HOST_DEFAULT CONNECTION_HOST_USB

// Persist a small keyboard config block (last Bluetooth slot) in the EEPROM-emulation flash.
#define EECONFIG_KB_DATA_SIZE    4
#define EECONFIG_KB_DATA_VERSION 1

// Idle sleep: backlight + RGB off after this long without key/knob/slider activity; any input wakes them.
#define DISPLAY_SLEEP_TIMEOUT_MS (3UL * 60UL * 1000UL)
