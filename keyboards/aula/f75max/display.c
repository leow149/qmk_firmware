// SPDX-License-Identifier: GPL-2.0-or-later
// AULA F75 Max LCD: 128x128 GC9107-compatible panel. The init sequence is the one the stock firmware sends
// (traced from the stock image, see lcd_init_f75max.txt), NOT QMK's default gc9107 init.
#include QMK_KEYBOARD_H
#include "qp.h"
#include "qp_comms.h"
#include "gpio.h"
#include "graphics/opensans11.qff.h"
#include "graphics/opensans14.qff.h"
#include "graphics/opensans22.qff.h"
#include "module.h"
#include "f75max.h"
#include "connection.h"
#include "usb_main.h"
#ifndef USB_GETSTATUS_REMOTE_WAKEUP_ENABLED
#    define USB_GETSTATUS_REMOTE_WAKEUP_ENABLED (2U)   // USB device-status bit 1 (same value QMK uses privately in chibios.c)
#endif
#include "bluetooth/ch582f_ajazz.h"

static painter_device_t lcd;
static painter_font_handle_t font11, font14, font22;

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
    uint32_t rgb;                    // (enabled << 16) | (hue << 8) | bar width in pixels
    uint8_t  page;                   // 0 = status, 1 = PC metrics
    uint8_t  m_cpu, m_cput, m_gpu, m_gput, m_ram;   // metrics (0xFF = not available); only filled while page == 1
    uint8_t  _pad;
    uint32_t m_dn, m_up;             // network rate in kB/s (0xFFFFFFFF = not available)
} ui_state_t;

// ---- PC metrics, pushed by the host over raw HID (packet: [0]=0xC0, [1]=version, [2]=cpu %, [3]=cpu C, [4]=gpu %, [5]=gpu C,
// [6]=ram %, [8..11]=down kB/s, [12..15]=up kB/s; 0xFF / 0xFFFFFFFF = not available). Stale after METRICS_TIMEOUT_MS. ----
#define HID_CMD_METRICS   0xC0
#define METRICS_TIMEOUT_MS 4000
#define NA8  0xFF
#define NA32 0xFFFFFFFFUL
static struct { uint8_t cpu, cput, gpu, gput, ram; uint32_t dn, up; uint32_t stamp; bool have; } metrics;
static uint8_t ui_page = 0;

bool f75max_hid_command(uint8_t *data, uint8_t length) {
    if (length < 16 || data[0] != HID_CMD_METRICS) return false;
    metrics.cpu = data[2]; metrics.cput = data[3]; metrics.gpu = data[4]; metrics.gput = data[5]; metrics.ram = data[6];
    metrics.dn  = (uint32_t)data[8]  | ((uint32_t)data[9]  << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    metrics.up  = (uint32_t)data[12] | ((uint32_t)data[13] << 8) | ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);
    metrics.stamp = timer_read32();
    metrics.have  = true;
    return true;
}
void display_toggle_page(void) { ui_page ^= 1; }

static ui_state_t shown;
static bool       shown_valid = false;       // false forces a full repaint

static uint16_t mode_key(void) {
    return ((uint16_t)conn_mode << 8) | ((uint16_t)ch582_get_conn_state() << 4) | ch582_get_target_slot();
}
static ui_state_t ui_now(void) {
    ui_state_t s;
    memset(&s, 0, sizeof(s));
    s.caps = host_keyboard_led_state().caps_lock ? 1 : 0;
    s.wl   = keymap_config.no_gui ? 1 : 0;
    s.fn   = layer_state_is(1) ? 1 : 0;
    s.os   = keymap_config.swap_lalt_lgui ? 1 : 0;      // Mac = Alt/Win swapped
    s.mode = mode_key();
    s.batt = ((uint16_t)module_battery() << 2) | (uint16_t)module_charge_state();
    s.rgb  = ((uint32_t)(rgb_matrix_is_enabled() ? 1 : 0) << 16) | ((uint32_t)rgb_matrix_get_hue() << 8) | (uint32_t)((rgb_matrix_get_val() * 88 + 127) / 255);
    s.page = ui_page;
    s.m_cpu = s.m_cput = s.m_gpu = s.m_gput = s.m_ram = NA8;
    s.m_dn = s.m_up = NA32;
    if (ui_page && metrics.have && timer_elapsed32(metrics.stamp) < METRICS_TIMEOUT_MS) {
        s.m_cpu = metrics.cpu; s.m_cput = metrics.cput; s.m_gpu = metrics.gpu; s.m_gput = metrics.gput; s.m_ram = metrics.ram;
        s.m_dn = metrics.dn; s.m_up = metrics.up;
    }
    return s;
}

#define DISPLAY_IDLE_MS 300     // redraw only after this long without key/knob input

// Palette (QMK HSV). Muted greys for structure, one saturated colour per state.
#define C_BG     0,   0,   0
#define C_CARD   0,   0,  24
#define C_TRACK  0,   0,  42
#define C_TXT    0,   0, 235
#define C_TXT2   0,   0, 140
#define C_TXT3   0,   0,  95
#define C_ACCENT 150, 190, 255
#define C_OK     88,  200, 235
#define C_WARN   28,  235, 255
#define C_BAD    0,   230, 255
#define C_PAIR   160, 200, 255
#define C_MAC    213, 150, 255

// Rounded rectangle from two rectangles and four circles (inclusive coordinates, like qp_rect).
static void rrect(int16_t l, int16_t t, int16_t r, int16_t b, int16_t rad, uint8_t h, uint8_t s, uint8_t v) {
    qp_rect(lcd, l + rad, t, r - rad, b, h, s, v, true);
    qp_rect(lcd, l, t + rad, r, b - rad, h, s, v, true);
    qp_circle(lcd, l + rad, t + rad, rad, h, s, v, true);
    qp_circle(lcd, r - rad, t + rad, rad, h, s, v, true);
    qp_circle(lcd, l + rad, b - rad, rad, h, s, v, true);
    qp_circle(lcd, r - rad, b - rad, rad, h, s, v, true);
}
#define RRECT(l, t, r, b, rad, col) rrect(l, t, r, b, rad, col)
#define TEXT(x, y, f, str, ...) qp_drawtext_recolor(lcd, x, y, f, str, __VA_ARGS__)

// Link colour and caption for the current mode.
static void link_style(const ui_state_t *s, uint8_t col[3], const char **caption, uint8_t cap_col[3]) {
    conn_mode_t m = (conn_mode_t)(s->mode >> 8);
    ch582_conn_state_t st = (ch582_conn_state_t)((s->mode >> 4) & 0xF);
    static const uint8_t k_accent[3] = {C_ACCENT}, k_ok[3] = {C_OK}, k_pair[3] = {C_PAIR}, k_bad[3] = {C_BAD}, k_warn[3] = {C_WARN}, k_t2[3] = {C_TXT2};
    const uint8_t *c, *cc;
    const char *t;
    if (m == MODE_USB)                    { c = k_accent; t = "USB cable";  cc = k_t2;   }
    else if (st == CH582_CONN_CONNECTED)  { c = k_ok;     t = "Connected";  cc = k_ok;   }
    else if (st == CH582_CONN_PAIRING)    { c = k_pair;   t = "Pairing..."; cc = k_pair; }
    else if (st == CH582_CONN_REJECTED)   { c = k_bad;    t = "Rejected";   cc = k_bad;  }
    else if (st == CH582_CONN_LINKING)    { c = k_warn;   t = "Linking..."; cc = k_warn; }
    else                                  { c = k_warn;   t = "Idle";       cc = k_warn; }
    memcpy(col, c, 3); memcpy(cap_col, cc, 3); *caption = t;
}

// Every element repaints only its own pixels, with opaque backgrounds and no clear-to-black first, so a change never
// shows an intermediate state. A full repaint (first frame) clears the panel once.

// --- header, left: pill with connection icon + label ---
static void draw_conn(const ui_state_t *s) {
    conn_mode_t m = (conn_mode_t)(s->mode >> 8);
    uint8_t slot = s->mode & 0xF;
    uint8_t col[3], cap_col[3]; const char *cap;
    link_style(s, col, &cap, cap_col);
    char t[12];
    if (m == MODE_USB) snprintf(t, sizeof(t), "USB");
    else if (m == MODE_BT) snprintf(t, sizeof(t), "BT %u", slot);
    else snprintf(t, sizeof(t), "2.4G");
    int16_t pw = 8 + 14 + 5 + qp_textwidth(font14, t) + 8;
    if (4 + pw + 1 <= 69) qp_rect(lcd, 4 + pw + 1, 4, 69, 25, C_BG, true);       // only the part a longer pill used to cover
    RRECT(4, 4, 4 + pw, 25, 10, C_CARD);
    const int16_t ox = 10, oy = 8;
    uint8_t h = col[0], sa = col[1], v = col[2];
    if (m == MODE_USB) {                                          // plug
        qp_rect(lcd, ox + 3, oy + 5, ox + 10, oy + 9, h, sa, v, true);
        qp_rect(lcd, ox + 4, oy + 1, ox + 5,  oy + 5, h, sa, v, true);
        qp_rect(lcd, ox + 8, oy + 1, ox + 9,  oy + 5, h, sa, v, true);
        qp_rect(lcd, ox + 6, oy + 9, ox + 7,  oy + 13, h, sa, v, true);
    } else if (m == MODE_BT) {                                    // Bluetooth rune, 2 px strokes
        for (int16_t dx = 0; dx < 2; dx++) {
            qp_line(lcd, ox + 3 + dx,  oy + 3,  ox + 10 + dx, oy + 9,  h, sa, v);
            qp_line(lcd, ox + 10 + dx, oy + 9,  ox + 7 + dx,  oy + 13, h, sa, v);
            qp_line(lcd, ox + 7 + dx,  oy + 13, ox + 7 + dx,  oy + 0,  h, sa, v);
            qp_line(lcd, ox + 7 + dx,  oy + 0,  ox + 10 + dx, oy + 4,  h, sa, v);
            qp_line(lcd, ox + 10 + dx, oy + 4,  ox + 3 + dx,  oy + 10, h, sa, v);
        }
    } else {                                                      // signal bars (2.4 GHz)
        static const uint8_t bh[4] = {4, 7, 10, 13};
        for (int16_t i = 0; i < 4; i++) qp_rect(lcd, ox + i * 3, oy + 13 - bh[i], ox + i * 3 + 1, oy + 13, h, sa, v, true);
    }
    TEXT(ox + 19, 8, font14, t, C_TXT, C_CARD);
}

// --- header, right: battery gauge, percent, charging bolt ---
#define BATT_X 102
#define BATT_Y 9
static void draw_batt_frame(void) {
    rrect(BATT_X, BATT_Y, BATT_X + 20, BATT_Y + 11, 2, C_TXT2);
    rrect(BATT_X + 1, BATT_Y + 1, BATT_X + 19, BATT_Y + 10, 1, C_BG);
    qp_rect(lcd, BATT_X + 21, BATT_Y + 3, BATT_X + 22, BATT_Y + 8, C_TXT2, true);   // nub
}
static void draw_batt(const ui_state_t *s) {
    uint8_t pct = s->batt >> 2;
    charge_state_t cs = (charge_state_t)(s->batt & 3);
    if (pct == BATT_UNKNOWN) {
        qp_rect(lcd, 70, 5, PANEL_WIDTH - 1, 24, C_BG, true);
        TEXT(112, 9, font11, "--", C_TXT3, C_BG);
        return;
    }
    draw_batt_frame();
    int16_t w = ((int16_t)pct * 17 + 50) / 100;
    uint8_t hue = pct > 50 ? 88 : pct > 20 ? 28 : 0;
    uint8_t sat = pct > 50 ? 200 : pct > 20 ? 235 : 230, val = pct > 50 ? 235 : 255;
    const int16_t ix = BATT_X + 2, iy = BATT_Y + 2;                              // interior: 17 x 8 px
    if (w > 0)  qp_rect(lcd, ix, iy, ix + w - 1, iy + 7, hue, sat, val, true);
    if (w < 17) qp_rect(lcd, ix + w, iy, ix + 16, iy + 7, C_BG, true);
    if (cs == CHG_CHARGING) {                                                    // bolt over the gauge
        qp_line(lcd, BATT_X + 11, BATT_Y + 1, BATT_X + 7,  BATT_Y + 6,  C_TXT);
        qp_line(lcd, BATT_X + 7,  BATT_Y + 6, BATT_X + 12, BATT_Y + 6,  C_TXT);
        qp_line(lcd, BATT_X + 12, BATT_Y + 6, BATT_X + 8,  BATT_Y + 11, C_TXT);
    }
    char t[8];
    snprintf(t, sizeof(t), "%u%%", pct);
    int16_t tw = qp_textwidth(font11, t), tx = BATT_X - 4 - tw;
    if (tx > 70) qp_rect(lcd, 70, 5, tx - 1, 24, C_BG, true);                    // only the part a wider number used to cover
    if (cs == CHG_NONE && pct <= 10) TEXT(tx, 9, font11, t, C_BAD, C_BG);
    else                             TEXT(tx, 9, font11, t, C_TXT2, C_BG);
}

// --- hero card ---
static void draw_card(void) { RRECT(4, 31, 123, 88, 9, C_CARD); }
static void draw_logo(const ui_state_t *s) {
    qp_rect(lcd, 12, 37, 45, 68, C_CARD, true);                                  // inside the card, behind the logo
    if (!s->os) {                                                                // Windows-style four panes
        static const int16_t px[2] = {14, 29}, py[2] = {39, 54};
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) qp_rect(lcd, px[i], py[j], px[i] + 12, py[j] + 12, C_ACCENT, true);
    } else {                                                                     // keycap with "cmd", same footprint as the Windows logo
        rrect(14, 39, 41, 66, 6, C_MAC);
        rrect(16, 41, 39, 64, 5, 213, 150, 60);
        int16_t cw = qp_textwidth(font11, "cmd");
        TEXT(14 + (28 - cw) / 2, 48, font11, "cmd", 213, 60, 255, 213, 150, 60);
    }
}
static void draw_name(const ui_state_t *s) {
    qp_rect(lcd, 52, 40, 118, 68, C_CARD, true);
    TEXT(56, 44, font22, s->os ? "MAC" : "WIN", C_TXT, C_CARD);
}
static void draw_caption(const ui_state_t *s) {
    uint8_t col[3], cc[3]; const char *cap;
    link_style(s, col, &cap, cc);
    qp_rect(lcd, 8, 70, 119, 84, C_CARD, true);
    int16_t tw = qp_textwidth(font11, cap);
    TEXT(4 + (120 - tw) / 2, 72, font11, cap, cc[0], cc[1], cc[2], C_CARD);
}

// --- RGB bar: current lighting colour and brightness (two rectangles, no flash while it changes) ---
#define RGB_BAR_X 34
#define RGB_BAR_W 88
static void draw_rgb_label(void) { TEXT(6, 93, font11, "RGB", C_TXT3, C_BG); }
static void draw_rgb(const ui_state_t *s, const ui_state_t *prev) {
    uint8_t hue = (s->rgb >> 8) & 0xFF, w = s->rgb & 0xFF;
    bool on = (s->rgb >> 16) & 1;
    bool was_on = prev && ((prev->rgb >> 16) & 1);
    if (!on) {
        qp_rect(lcd, RGB_BAR_X, 91, PANEL_WIDTH - 1, 104, C_BG, true);
        TEXT(RGB_BAR_X, 93, font11, "off", C_TXT3, C_BG);
        return;
    }
    if (!was_on) qp_rect(lcd, RGB_BAR_X, 91, PANEL_WIDTH - 1, 104, C_BG, true);  // leaving "off" / first paint
    if (w > RGB_BAR_W) w = RGB_BAR_W;
    if (w > 0)           qp_rect(lcd, RGB_BAR_X, 96, RGB_BAR_X + w - 1, 101, hue, 255, 255, true);
    if (w < RGB_BAR_W)   qp_rect(lcd, RGB_BAR_X + w, 96, RGB_BAR_X + RGB_BAR_W - 1, 101, C_TRACK, true);
}

// --- bottom: status chips, each repainted on its own ---
static void chip(int16_t x, int16_t w, const char *label, bool on, uint8_t hue, uint8_t sat, uint8_t val) {
    int16_t tw = qp_textwidth(font11, label);
    if (on) { rrect(x, 108, x + w - 1, 124, 8, hue, sat, val); TEXT(x + (w - tw) / 2, 110, font11, label, C_BG, hue, sat, val); }
    else    { RRECT(x, 108, x + w - 1, 124, 8, C_CARD);        TEXT(x + (w - tw) / 2, 110, font11, label, C_TXT3, C_CARD); }
}
static void draw_chip_caps(const ui_state_t *s) { chip(4,  48, "CAPS", s->caps, C_WARN); }
static void draw_chip_wl(const ui_state_t *s)   { chip(56, 32, "WL",   s->wl,   C_BAD);  }
static void draw_chip_fn(const ui_state_t *s)   { chip(92, 32, "FN",   s->fn,   C_ACCENT); }

// --- metrics page: CPU / GPU / RAM rows and a network row. Static parts (cards, labels) are drawn once per page switch;
// updates only touch bars and value boxes, so a number changing never flashes the row. ---
#define PINK_GPU 200, 170, 255
static const int16_t ROW_Y[3] = {31, 56, 81};
#define NET_Y 106

static void temp_color(uint8_t t, uint8_t *h, uint8_t *sa, uint8_t *v) {
    if (t < 60)      { *h = 88; *sa = 200; *v = 235; }
    else if (t < 80) { *h = 28; *sa = 235; *v = 255; }
    else             { *h = 0;  *sa = 230; *v = 255; }
}
static void draw_row_static(int row, const char *label) {
    rrect(4, ROW_Y[row], 123, ROW_Y[row] + 21, 6, C_CARD);
    TEXT(9, ROW_Y[row] + 5, font11, label, C_TXT2, C_CARD);
}
// pct / temp: 0xFF = not available. wide: RAM row (longer bar, "%" sign, no temperature).
static void draw_row_values(int row, uint8_t pct, uint8_t temp, bool wide, uint8_t h, uint8_t sa, uint8_t v) {
    const int16_t y = ROW_Y[row], bx0 = 41, bx1 = wide ? 91 : 70, n = bx1 - bx0 + 1;
    int16_t w = (pct == NA8) ? 0 : (int16_t)(pct > 100 ? 100 : pct) * n / 100;
    if (w > 0) qp_rect(lcd, bx0, y + 8, bx0 + w - 1, y + 13, h, sa, v, true);
    if (w < n) qp_rect(lcd, bx0 + w, y + 8, bx1, y + 13, C_TRACK, true);
    char t[8];
    if (pct == NA8) snprintf(t, sizeof(t), "--");
    else if (wide)  snprintf(t, sizeof(t), "%u%%", pct);
    else            snprintf(t, sizeof(t), "%u", pct);
    int16_t xr = wide ? 118 : 96, tw = qp_textwidth(font11, t);
    qp_rect(lcd, bx1 + 2, y + 3, xr + 1, y + 18, C_CARD, true);                 // value box (right of the bar)
    if (pct == NA8) TEXT(xr - tw, y + 5, font11, t, C_TXT3, C_CARD);
    else            TEXT(xr - tw, y + 5, font11, t, C_TXT,  C_CARD);
    if (!wide) {
        qp_rect(lcd, 98, y + 3, 120, y + 18, C_CARD, true);                     // temperature box
        if (temp != NA8) {
            uint8_t th, ts, tv; temp_color(temp, &th, &ts, &tv);
            snprintf(t, sizeof(t), "%u", temp);
            int16_t ttw = qp_textwidth(font11, t), x = 115 - ttw;
            TEXT(x, y + 5, font11, t, th, ts, tv, C_CARD);
            qp_circle(lcd, x + ttw + 2, y + 7, 1, th, ts, tv, false);           // degree sign (the font has no glyph for it)
        }
    }
}
static void fmt_rate(char *out, size_t n, uint32_t k) {                          // kB/s -> "850K" / "1.2M" / "125M"
    if (k == NA32)      snprintf(out, n, "--");
    else if (k < 1000)  snprintf(out, n, "%luK", (unsigned long)k);
    else if (k < 100000) snprintf(out, n, "%lu.%luM", (unsigned long)(k / 1000), (unsigned long)((k % 1000) / 100));
    else                snprintf(out, n, "%luM", (unsigned long)((k + 500) / 1000));
}
static void draw_tri(int16_t x, int16_t y, bool down, uint8_t h, uint8_t sa, uint8_t v) {
    for (int i = 0; i < 4; i++) {
        int16_t yy = down ? y + i : y + 3 - i;
        qp_line(lcd, x + i, yy, x + 8 - i, yy, h, sa, v);
    }
}
static void draw_net_static(void) {
    rrect(4, NET_Y, 123, NET_Y + 18, 6, C_CARD);
    draw_tri(10, NET_Y + 7, true,  C_ACCENT);
    draw_tri(68, NET_Y + 7, false, 28, 200, 255);
}
static void draw_net_values(uint32_t dn, uint32_t up) {
    char t[10];
    qp_rect(lcd, 21, NET_Y + 3, 62, NET_Y + 15, C_CARD, true);
    fmt_rate(t, sizeof(t), dn);
    if (dn == NA32) TEXT(22, NET_Y + 3, font11, t, C_TXT3, C_CARD);
    else            TEXT(22, NET_Y + 3, font11, t, C_TXT,  C_CARD);
    qp_rect(lcd, 79, NET_Y + 3, 120, NET_Y + 15, C_CARD, true);
    fmt_rate(t, sizeof(t), up);
    if (up == NA32) TEXT(80, NET_Y + 3, font11, t, C_TXT3, C_CARD);
    else            TEXT(80, NET_Y + 3, font11, t, C_TXT,  C_CARD);
}
static void draw_metrics_static(void) {
    draw_row_static(0, "CPU"); draw_row_static(1, "GPU"); draw_row_static(2, "RAM"); draw_net_static();
}
static void draw_metrics_values(const ui_state_t *s, const ui_state_t *prev) {
    if (!prev || s->m_cpu != prev->m_cpu || s->m_cput != prev->m_cput) draw_row_values(0, s->m_cpu, s->m_cput, false, C_ACCENT);
    if (!prev || s->m_gpu != prev->m_gpu || s->m_gput != prev->m_gput) draw_row_values(1, s->m_gpu, s->m_gput, false, PINK_GPU);
    if (!prev || s->m_ram != prev->m_ram)                                draw_row_values(2, s->m_ram, NA8, true, C_OK);
    if (!prev || s->m_dn != prev->m_dn || s->m_up != prev->m_up)         draw_net_values(s->m_dn, s->m_up);
}

// --- body of the status page (everything under the header) ---
static void draw_status_body(const ui_state_t *s) {
    draw_card();
    draw_logo(s);
    draw_name(s);
    draw_caption(s);
    draw_rgb_label();
    draw_rgb(s, NULL);
    draw_chip_caps(s);
    draw_chip_wl(s);
    draw_chip_fn(s);
}
static void draw_body(const ui_state_t *s) {
    if (s->page) { draw_metrics_static(); draw_metrics_values(s, NULL); }
    else         { draw_status_body(s); }
}

// First frame (or after an explicit invalidate): clear the panel once (optionally), then paint everything.
static void draw_full(const ui_state_t *s, bool clear) {
    if (clear) qp_rect(lcd, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, C_BG, true);
    draw_conn(s);
    draw_batt(s);
    draw_body(s);
}

// Paint only what differs from what the panel already shows.
static void draw_state(void) {
    ui_state_t n = ui_now();
    if (!shown_valid) {
        draw_full(&n, true);
        shown_valid = true;
    } else {
        if (n.mode != shown.mode) { draw_conn(&n); if (!n.page) draw_caption(&n); }
        if (n.batt != shown.batt) draw_batt(&n);
        if (n.page != shown.page) {                                  // page switch: clear the body once, paint the new page
            qp_rect(lcd, 0, 28, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, C_BG, true);
            draw_body(&n);
        } else if (!n.page) {
            if (n.os != shown.os)     { draw_logo(&n); draw_name(&n); }
            if (n.rgb != shown.rgb)   draw_rgb(&n, &shown);
            if (n.caps != shown.caps) draw_chip_caps(&n);
            if (n.wl != shown.wl)     draw_chip_wl(&n);
            if (n.fn != shown.fn)     draw_chip_fn(&n);
        } else {
            draw_metrics_values(&n, &shown);
        }
    }
    shown = n;
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
        draw_state();                                 // the panel kept its picture: paint only what changed while asleep
        gpio_write_pin_high(PANEL_BKL);
    }
}
static uint32_t activity_elapsed(void) {
    uint32_t a = last_input_activity_elapsed(), b = timer_elapsed32(slider_activity);
    return a < b ? a : b;
}

void display_bootloader_notice(void) {
    if (sleeping) gpio_write_pin_high(PANEL_BKL);
    qp_rect(lcd, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, C_BG, true);
    RRECT(4, 31, 123, 88, 9, C_CARD);
    int16_t w1 = qp_textwidth(font14, "BOOTLOADER"), w2 = qp_textwidth(font11, "ready to flash");
    TEXT((PANEL_WIDTH - w1) / 2, 44, font14, "BOOTLOADER", C_WARN, C_CARD);
    TEXT((PANEL_WIDTH - w2) / 2, 66, font11, "ready to flash", C_TXT2, C_CARD);
    qp_flush(lcd);
}

void keyboard_post_init_kb(void) {
    // Stock firmware: SPI mode 0, 8-bit, manual CS. Mode 3 (as on the AK820) gave a blank panel here. Divisor 4 is verified.
    // QMK's built-in reset waits only 20 ms after RST rises; these panels need ~120 ms before the first command, otherwise the
    // early commands (sleep-out, gamma, ...) are lost after a soft reboot that left the panel powered. So we pulse RST ourselves.
    gpio_set_pin_output(PANEL_RST);
    gpio_write_pin_high(PANEL_RST);
    wait_ms(5);
    gpio_write_pin_low(PANEL_RST);
    wait_ms(20);
    gpio_write_pin_high(PANEL_RST);
    wait_ms(150);
    lcd = qp_gc9107_make_spi_device(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CS, PANEL_DC, NO_PIN /*reset done above*/, 4 /*divisor*/, 0 /*spi mode*/);
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
    font11 = qp_load_font_mem(font_opensans11);
    font14 = qp_load_font_mem(font_opensans14);
    font22 = qp_load_font_mem(font_opensans22);
    // First frame is drawn with the backlight off; the stock raises P0.16 afterwards.
    draw_state();
    gpio_write_pin_high(PANEL_BKL);    // backlight is ACTIVE HIGH on this board
    module_init();
    keyboard_post_init_user();
}

void housekeeping_task_kb(void) {
    f75max_recovery_task();          // first: must work whatever else is going on
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
        ui_state_t n = ui_now();
        draw_full(&n, false);                           // over the existing picture: invisible if the first frame arrived
        shown = n; shown_valid = true;
        qp_flush(lcd);
    }
    if (timer_elapsed32(last) < 100) return;
    last = timer_read32();
    // Never block the main loop with SPI traffic while typing: wireless key frames are sent from this loop.
    if (activity_elapsed() < DISPLAY_IDLE_MS) return;
    ui_state_t n = ui_now();
    if (!shown_valid || memcmp(&n, &shown, sizeof(n)) != 0) draw_state();
}
