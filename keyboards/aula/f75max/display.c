// SPDX-License-Identifier: GPL-2.0-or-later
// AULA F75 Max LCD: 128x128 GC9107-compatible panel. The init sequence is the one the stock firmware sends
// (traced from the stock image, see lcd_init_f75max.txt), NOT QMK's default gc9107 init.
#include QMK_KEYBOARD_H
#include "qp.h"
#include "qp_comms.h"
#include "gpio.h"
#include "graphics/opensans14.qff.h"
#include "graphics/opensans34.qff.h"
#include "module.h"
#include "f75max.h"
#include "connection.h"
#include "usb_main.h"
#ifndef USB_GETSTATUS_REMOTE_WAKEUP_ENABLED
#    define USB_GETSTATUS_REMOTE_WAKEUP_ENABLED (2U)   // USB device-status bit 1 (same value QMK uses privately in chibios.c)
#endif
#include "bluetooth/ch582f_ajazz.h"

static painter_device_t lcd;
static painter_font_handle_t font14, font34;

// Stock init, in stock order. Entry format for qp_comms_bulk_command_sequence: { command, delay_ms_after, nbytes, bytes... }
bool qp_gc9107_init(painter_device_t device, painter_rotation_t rotation) {
    static const uint8_t seq1[] = {
        0x11, 120, 0,                                   // sleep out
        0xB1, 0, 3, 0x05, 0x3A, 0x3A,                   // frame rate (normal)
        0xB2, 0, 3, 0x05, 0x3A, 0x3A,                   // frame rate (idle)
        0xB3, 0, 6, 0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A, // frame rate (partial)
        0xB4, 0, 1, 0x03,                               // inversion control
        0xC0, 0, 3, 0x62, 0x02, 0x04,                   // power control 1
        0xC1, 0, 1, 0xC0,                               // power control 2
        0xC2, 0, 2, 0x0D, 0x00,                         // power control 3
        0xC3, 0, 2, 0x8D, 0x6A,                         // power control 4
        0xC4, 0, 2, 0x8D, 0xEE,                         // power control 5
        0xC5, 0, 1, 0x12,                               // VCOM
    };
    static const uint8_t seq2[] = {
        0xE0, 0, 16, 0x03, 0x1B, 0x12, 0x11, 0x3F, 0x3A, 0x32, 0x34, 0x2F, 0x2B, 0x30, 0x3A, 0x00, 0x01, 0x02, 0x05, // gamma +
        0xE1, 0, 16, 0x03, 0x1B, 0x12, 0x11, 0x32, 0x2F, 0x2A, 0x2F, 0x2E, 0x2C, 0x35, 0x3F, 0x00, 0x00, 0x01, 0x05, // gamma -
        0x3A, 0, 1, 0x05,                               // 16 bpp
        0x29, 20, 0,                                    // display on
        0x21, 0, 0,                                     // inversion on (stock does this)
    };
    if (!qp_comms_bulk_command_sequence(device, seq1, sizeof(seq1))) return false;
    // MADCTL: the stock firmware uses 0xD8 (MY|MX|ML|BGR). We expose it as QP_ROTATION_180.
    if (!qp_comms_command_databyte(device, 0x36, 0xD8)) return false;
    return qp_comms_bulk_command_sequence(device, seq2, sizeof(seq2));
}


// ---- connection-mode slider: A10 low = 2.4G, B2 low = Bluetooth, both high = USB (verified on hardware) ----
static uint32_t slider_activity = 0;     // timer value at the last slider change (counts as activity)
static conn_mode_t conn_mode = MODE_USB;      // debounced slider position (the active mode lives in f75max.c)

static int8_t read_slider_raw(void) {           // -1 = invalid combination (both low)
    bool a10 = gpio_read_pin(A10), b2 = gpio_read_pin(B2);
    if (!a10 && !b2) return -1;
    if (!a10) return MODE_24G;
    if (!b2)  return MODE_BT;
    return MODE_USB;
}
static void slider_task(void) {
    static int8_t cand = -2; static uint32_t since = 0;
    int8_t raw = read_slider_raw();
    if (raw < 0) return;                        // ignore the transition between positions
    if (raw != cand) { cand = raw; since = timer_read32(); return; }
    if ((int8_t)conn_mode != raw && timer_elapsed32(since) >= 30) { conn_mode = (conn_mode_t)raw; slider_activity = timer_read32(); f75max_apply_mode(conn_mode); }
}

// ---------------------------------------------------------------------------------------------------------------
// UI state and drawing. Everything is drawn from primitives + two generated fonts; nothing is copied from stock graphics.
// ---------------------------------------------------------------------------------------------------------------
typedef struct {
    uint8_t  caps, wl, fn, os;       // Caps Lock, Windows-key lock, Fn layer held, layout (0 = Win/Android, 1 = Mac)
    uint16_t mode;                   // slider mode, link state, target slot
    uint16_t batt;                   // (percent << 2) | charge_state
} ui_state_t;

static ui_state_t shown;
static bool       shown_valid = false;       // false forces a full repaint

static uint16_t mode_key(void) {
    return ((uint16_t)conn_mode << 8) | ((uint16_t)ch582_get_conn_state() << 4) | ch582_get_target_slot();
}
static ui_state_t ui_now(void) {
    ui_state_t s;
    s.caps = host_keyboard_led_state().caps_lock ? 1 : 0;
    s.wl   = keymap_config.no_gui ? 1 : 0;
    s.fn   = layer_state_is(2) ? 1 : 0;
    s.os   = (default_layer_state & (1UL << 1)) ? 1 : 0;
    s.mode = mode_key();
    s.batt = ((uint16_t)module_battery() << 2) | (uint16_t)module_charge_state();
    return s;
}

#define DISPLAY_IDLE_MS 300     // redraw only after this long without key/knob input
#define COL_WHITE   0, 0, 255
#define COL_DIM     0, 0, 90
#define COL_BLACK   0, 0, 0

static void text_centered(painter_font_handle_t f, int16_t y, const char *s, uint8_t h, uint8_t sa, uint8_t v) {
    int16_t w = qp_textwidth(f, s);
    qp_drawtext_recolor(lcd, (PANEL_WIDTH - w) / 2, y, f, s, h, sa, v, COL_BLACK);
}

// --- header, left: connection icon + label ---
static void draw_conn(const ui_state_t *s, bool clear) {
    conn_mode_t m = (conn_mode_t)(s->mode >> 8);
    ch582_conn_state_t st = (ch582_conn_state_t)((s->mode >> 4) & 0xF);
    uint8_t slot = s->mode & 0xF;
    bool ok = (m == MODE_USB) || st == CH582_CONN_CONNECTED;
    uint8_t hue = (m == MODE_USB) ? 170 : st == CH582_CONN_CONNECTED ? 85 : st == CH582_CONN_REJECTED ? 0 : 43;   // blue / green / red / yellow
    if (clear) qp_rect(lcd, 0, 0, 63, 23, COL_BLACK, true);
    const int16_t ox = 4, oy = 4;                                 // 16x16 icon box
    if (m == MODE_USB) {                                          // plug
        qp_rect(lcd, ox + 4, oy + 5, ox + 11, oy + 10, hue, 255, 255, true);
        qp_rect(lcd, ox + 5, oy + 1, ox + 6,  oy + 5,  hue, 255, 255, true);
        qp_rect(lcd, ox + 9, oy + 1, ox + 10, oy + 5,  hue, 255, 255, true);
        qp_rect(lcd, ox + 7, oy + 10, ox + 8, oy + 15, hue, 255, 255, true);
    } else if (m == MODE_BT) {                                    // Bluetooth rune
        qp_line(lcd, ox + 4,  oy + 4,  ox + 12, oy + 11, hue, 255, 255);
        qp_line(lcd, ox + 12, oy + 11, ox + 8,  oy + 15, hue, 255, 255);
        qp_line(lcd, ox + 8,  oy + 15, ox + 8,  oy + 1,  hue, 255, 255);
        qp_line(lcd, ox + 8,  oy + 1,  ox + 12, oy + 5,  hue, 255, 255);
        qp_line(lcd, ox + 12, oy + 5,  ox + 4,  oy + 12, hue, 255, 255);
    } else {                                                      // signal bars (2.4 GHz)
        qp_rect(lcd, ox + 1,  oy + 11, ox + 3,  oy + 15, hue, 255, 255, true);
        qp_rect(lcd, ox + 5,  oy + 8,  ox + 7,  oy + 15, hue, 255, 255, true);
        qp_rect(lcd, ox + 9,  oy + 5,  ox + 11, oy + 15, hue, 255, 255, true);
        qp_rect(lcd, ox + 13, oy + 2,  ox + 15, oy + 15, hue, 255, 255, true);
    }
    char t[12];
    if (m == MODE_USB) snprintf(t, sizeof(t), "USB");
    else if (m == MODE_BT) snprintf(t, sizeof(t), "BT %u", slot);
    else snprintf(t, sizeof(t), "2.4G");
    qp_drawtext_recolor(lcd, 25, 3, font14, t, ok ? 0 : hue, ok ? 0 : 255, 255, COL_BLACK);
}

// --- header, right: battery gauge (+ percent, charging bolt) ---
static void draw_batt(const ui_state_t *s, bool clear) {
    uint8_t pct = s->batt >> 2;
    charge_state_t cs = (charge_state_t)(s->batt & 3);
    if (clear) qp_rect(lcd, 64, 0, PANEL_WIDTH - 1, 23, COL_BLACK, true);
    const int16_t x0 = 96, y0 = 5;
    qp_rect(lcd, x0, y0, x0 + 25, y0 + 12, COL_WHITE, false);               // body
    qp_rect(lcd, x0 + 26, y0 + 3, x0 + 28, y0 + 9, COL_WHITE, true);        // nub
    qp_rect(lcd, x0 + 2, y0 + 2, x0 + 23, y0 + 10, COL_BLACK, true);        // inside
    char t[8];
    if (pct == BATT_UNKNOWN) {
        qp_drawtext_recolor(lcd, x0 + 8, y0 - 3, font14, "?", COL_DIM, COL_BLACK);
        qp_rect(lcd, 60, 3, x0 - 4, 20, COL_BLACK, true);
        return;
    }
    uint8_t hue = pct > 50 ? 85 : pct > 20 ? 43 : 0;                         // green / yellow / red
    int16_t w = (int16_t)pct * 22 / 100;
    if (w > 0) qp_rect(lcd, x0 + 2, y0 + 2, x0 + 1 + w, y0 + 10, hue, 255, 255, true);
    if (cs == CHG_CHARGING) {                                                // bolt over the gauge
        qp_line(lcd, x0 + 14, y0 + 2,  x0 + 10, y0 + 7,  COL_WHITE);
        qp_line(lcd, x0 + 10, y0 + 7,  x0 + 15, y0 + 7,  COL_WHITE);
        qp_line(lcd, x0 + 15, y0 + 7,  x0 + 11, y0 + 11, COL_WHITE);
    }
    snprintf(t, sizeof(t), "%u%%", pct);
    int16_t tw = qp_textwidth(font14, t);
    qp_rect(lcd, 60, 3, x0 - 4, 20, COL_BLACK, true);                        // clear the percent area (also on refresh: width can change)
    qp_drawtext_recolor(lcd, x0 - 5 - tw, 3, font14, t, cs == CHG_NONE && pct <= 10 ? 0 : 0, cs == CHG_NONE && pct <= 10 ? 255 : 0, 255, COL_BLACK);
}

// --- centre: layout name, with the link state underneath ---
static void draw_center(const ui_state_t *s, bool clear) {
    if (clear) qp_rect(lcd, 0, 30, PANEL_WIDTH - 1, 94, COL_BLACK, true);
    text_centered(font34, 30, s->os ? "MAC" : "WIN", s->os ? 213 : 170, 255, 255);
    conn_mode_t m = (conn_mode_t)(s->mode >> 8);
    ch582_conn_state_t st = (ch582_conn_state_t)((s->mode >> 4) & 0xF);
    const char *c;
    uint8_t hue = 43;
    if (m == MODE_USB)                    { c = "USB cable";    hue = 170; }
    else if (st == CH582_CONN_CONNECTED)  { c = "Connected";    hue = 85;  }
    else if (st == CH582_CONN_PAIRING)    { c = "Pairing...";   hue = 170; }
    else if (st == CH582_CONN_LINKING)    { c = "Linking...";   hue = 43;  }
    else if (st == CH582_CONN_REJECTED)   { c = "Rejected";     hue = 0;   }
    else                                  { c = "Idle";         hue = 43;  }
    text_centered(font14, 76, c, hue, 255, 255);
}

// --- bottom: status chips ---
static void chip(int16_t x, int16_t w, const char *label, bool on, uint8_t hue) {
    qp_rect(lcd, x, 100, x + w - 1, 121, on ? hue : 0, on ? 255 : 0, on ? 200 : 70, on);   // filled when active, dim outline otherwise
    if (!on) qp_rect(lcd, x + 1, 101, x + w - 2, 120, COL_BLACK, true), qp_rect(lcd, x, 100, x + w - 1, 121, COL_DIM, false);
    int16_t tw = qp_textwidth(font14, label);
    qp_drawtext_recolor(lcd, x + (w - tw) / 2, 102, font14, label, COL_WHITE, on ? hue : 0, on ? 200 : 0, on ? 0 : 0);
}
static void draw_chips(const ui_state_t *s, bool clear) {
    (void)clear;                                           // chips repaint their own background
    chip(5,  42, "CAPS", s->caps, 0);
    chip(55, 30, "WL",   s->wl,   10);
    chip(93, 30, "FN",   s->fn,   170);
}

static void draw_all(const ui_state_t *s, bool clear) {
    draw_conn(s, clear);
    draw_batt(s, clear);
    qp_line(lcd, 4, 25, PANEL_WIDTH - 5, 25, 0, 0, 80);
    draw_center(s, clear);
    draw_chips(s, clear);
}

// Redraw only what changed; a full repaint when nothing has been shown yet.
static void draw_state(void) {
    ui_state_t n = ui_now();
    if (!shown_valid) {
        qp_rect(lcd, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, COL_BLACK, true);
        draw_all(&n, false);
        shown_valid = true;
    } else {
        if (n.mode != shown.mode) { draw_conn(&n, true); draw_center(&n, true); }
        else if (n.os != shown.os) draw_center(&n, true);
        if (n.batt != shown.batt) draw_batt(&n, true);
        if (n.caps != shown.caps || n.wl != shown.wl || n.fn != shown.fn) draw_chips(&n, false);
    }
    shown = n;
    qp_flush(lcd);
}
// Self-healing: re-send everything WITHOUT clearing first (no flicker). A frame the panel dropped, e.g. after a soft
// reboot that left the panel powered, is repainted within the refresh interval.
static void draw_refresh(void) {
    ui_state_t n = ui_now();
    draw_all(&n, false);
    shown = n; shown_valid = true;
    qp_flush(lcd);
}

// ---- idle sleep: backlight + RGB off, wake on any input ----
static bool     sleeping = false;
static bool     low_batt_off = false;     // RGB forced off at <= 10% on battery
static void apply_rgb_suspend(void) { rgb_matrix_set_suspend_state(sleeping || low_batt_off); }
static void display_set_sleep(bool s) {
    if (s == sleeping) return;
    sleeping = s;
    if (s) {
        gpio_write_pin_low(PANEL_BKL);               // backlight off (active high)
        apply_rgb_suspend();
    } else {
        apply_rgb_suspend();
        shown_valid = false;                          // repaint everything on wake
        gpio_write_pin_high(PANEL_BKL);
    }
}
static uint32_t activity_elapsed(void) {
    uint32_t a = last_input_activity_elapsed(), b = timer_elapsed32(slider_activity);
    return a < b ? a : b;
}

void keyboard_post_init_kb(void) {
    // Stock firmware: SPI mode 0, 8-bit, manual CS. Mode 3 (as on the AK820) gave a blank panel here. Divisor 4 is verified.
    lcd = qp_gc9107_make_spi_device(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CS, PANEL_DC, PANEL_RST, 4 /*divisor*/, 0 /*spi mode*/);
    // window offsets default to x=2,y=1 in the driver, which is what the stock firmware uses.
    gpio_set_pin_input_high(A10);      // mode slider (read-only)
    gpio_set_pin_input_high(B2);
    { int8_t r = read_slider_raw(); if (r >= 0) conn_mode = (conn_mode_t)r; }
    f75max_load_config();              // restore the last Bluetooth slot and layout
    f75max_apply_mode(conn_mode);
    gpio_set_pin_output(PANEL_BKL);
    gpio_write_pin_low(PANEL_BKL);     // backlight OFF while the panel initialises (stock does the same)
    qp_init(lcd, QP_ROTATION_180);
    wait_ms(150);                      // let the panel settle after display-on before sending pixels
    font14 = qp_load_font_mem(font_opensans14);
    font34 = qp_load_font_mem(font_opensans34);
    // First frame is drawn with the backlight off; the stock raises P0.16 afterwards.
    draw_state();
    gpio_write_pin_high(PANEL_BKL);    // backlight is ACTIVE HIGH on this board
    module_init();
    keyboard_post_init_user();
}

void housekeeping_task_kb(void) {
    slider_task();
    module_task();
    {   // low battery: stop the RGB (the biggest load after the backlight) at <= 10% while on battery
        uint8_t pct = module_battery();
        bool low = pct != BATT_UNKNOWN && pct <= 10 && module_charge_state() == CHG_NONE;
        if (low != low_batt_off) { low_batt_off = low; apply_rgb_suspend(); }
    }
    {   // idle sleep / wake, also on a genuine USB bus suspend (the PC sleeping)
        bool host_suspended = (connection_get_host() == CONNECTION_HOST_USB) && (USB_DRIVER.state == USB_SUSPENDED);
        // QMK's own suspend loop is compiled out by the wireless driver (NO_USB_STARTUP_CHECK), so wake the PC ourselves.
        static uint32_t last_wake = 0;
        if (host_suspended && (USB_DRIVER.status & USB_GETSTATUS_REMOTE_WAKEUP_ENABLED) &&
            last_matrix_activity_elapsed() < 50 && timer_elapsed32(last_wake) > 500) {
            last_wake = timer_read32();
            usbWakeupHost(&USB_DRIVER);
        }
        bool idle = (timer_read32() > DISPLAY_SLEEP_TIMEOUT_MS && activity_elapsed() >= DISPLAY_SLEEP_TIMEOUT_MS) || host_suspended;
        display_set_sleep(idle);
        if (sleeping) return;
    }
    static uint32_t last = 0;
    static bool boot_redraw_done = false;
    if (!boot_redraw_done && timer_read32() > 1000) {   // a first frame sent too early can be lost: repaint once
        boot_redraw_done = true;
        shown_valid = false;
        draw_state();
    }
    if (timer_elapsed32(last) < 100) return;
    last = timer_read32();
    // Never block the main loop with SPI traffic while typing: wireless key frames are sent from this loop.
    if (activity_elapsed() < DISPLAY_IDLE_MS) return;
    ui_state_t n = ui_now();
    if (!shown_valid || memcmp(&n, &shown, sizeof(n)) != 0) draw_state();
    static uint32_t last_refresh = 0;
    if (timer_elapsed32(last_refresh) >= 3000) { last_refresh = timer_read32(); draw_refresh(); }
}
