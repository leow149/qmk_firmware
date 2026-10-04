// SPDX-License-Identifier: GPL-2.0-or-later
// module driver taken unchanged from Fernando Birra's (fpb) AJAZZ AK820 Pro port:
// https://github.com/fpb/qmk_firmware (branch ak820pro-lcd-flash, keyboards/a_jazz/ak820pro/bluetooth/)
// The AULA F75 Max wireless module speaks the same UART protocol (verified on hardware).
#include "ch582f_ajazz.h"
#include "bluetooth.h"
#include "quantum.h"
#include "hal.h"

#ifndef CH582_SERIAL_DRIVER
#    define CH582_SERIAL_DRIVER SD2
#endif

#define TX_MAX_PAYLOAD 16

/* Mirror the stock firmware's ACK behaviour: after receiving a 5B (connection
 * state) or 5C (battery) frame, the stock MCU replies with the 61 0D 0A ("a\r\n")
 * ACK token ~1.3 ms later. It does NOT ack 5A (LED) frames. Logic-analyzer-
 * confirmed against a stock BT boot. Experimental: enable to test whether a
 * "well-behaved" peer yields cleaner cold-boot connects or new frame types.
 * Define CH582_ACK_FRAMES=0 in config.h to disable. */
#ifndef CH582_ACK_FRAMES
#    define CH582_ACK_FRAMES 1
#endif

/* How often to re-issue the channel-select command while a connection is
 * requested but not yet established. At cold boot the dip-switch callback fires
 * a one-shot 0xA6 microseconds after sdStart(), before the CH582F has finished
 * its own power-up, so that first command is dropped. Retrying until the module
 * answers with a connect event makes boot-in-BT work. */
#ifndef CH582_CONNECT_RETRY_MS
#    define CH582_CONNECT_RETRY_MS 500
#endif

/* How often to poll the module for battery level. The module is silent in steady
 * state; the stock firmware periodically sends `A6 53` and the module answers with
 * a `5C <pct>` frame. (The LCD clock is NOT on this wire: the set-time utility
 * talks to the MCU directly, so time never crosses the CH582 serial link.) */
#ifndef CH582_BATTERY_POLL_MS
#    define CH582_BATTERY_POLL_MS 5000
#endif

/* The module's ONLY "connected" signal is a single, never-repeated 5B 32 (see
 * the frame-format note above ch582_service). This protocol has no ACKs and no
 * way to query state, so if that one frame is dropped the driver is stranded
 * in LINKING forever despite the link actually being up. Backstop: a 5A (host
 * LED) frame while LINKING is a side effect of an established link, so use it
 * to infer CONNECTED. Gated by a dwell so a real 5B 32 always wins first (it
 * would arrive well within this window on a healthy link). */
#ifndef CH582_5A_PROMOTE_MS
#    define CH582_5A_PROMOTE_MS 3000
#endif

/* Pairing (A6 51) is fire-and-forget like every other command on this link, so
 * a dropped frame silently strands hold-to-pair with no symptom to retry on
 * (see the BT fix notes: this read as a timing threshold, but wasn't one).
 * Resend until the module confirms via 5B 31/32 or we give up. */
#ifndef CH582_PAIR_RETRY_MS
#    define CH582_PAIR_RETRY_MS 400
#endif
#ifndef CH582_PAIR_MAX_TRIES
#    define CH582_PAIR_MAX_TRIES 12
#endif

/* The module ignores an A6 <slot> select for a slot that is already
 * advertising -- for the whole advertising window (minutes), not a brief
 * timing window. Naming a different slot forces an immediate state change,
 * so a same-slot reselect bounces through another slot first, then re-issues
 * the real target after this dwell (see the BT fix notes' hypothesis walk).
 * Known trade-off: the bounce slot can briefly connect (observed 5B 32 on the
 * bounce) -- unavoidable, since forcing the state change requires naming
 * another slot, the same exposure the manual workaround has. */
#ifndef CH582_BOUNCE_MS
#    define CH582_BOUNCE_MS 700
#endif

static volatile bool    is_module_connected = false;
/* Active BT slot 1-3. Derived from the selected 0xA6 profile, NOT from the 5B
 * stream: a logic-analyzer capture proved the 5B second byte is a handshake-STAGE
 * code (0x23/0x31..0x34 seen regardless of slot), not the slot number. 0 = none. */
static volatile uint8_t connected_slot = 0;

/* True while the module is advertising / waiting to bond (5B 31). */
static volatile bool    is_pairing = false;
/* Detailed connection state for the LCD indicator (superset of the two bools). */
static volatile ch582_conn_state_t conn_state = CH582_CONN_IDLE;
/* Battery percentage (0-100) from 5C frames; 0xFF until the module first reports. */
static volatile uint8_t battery_level = 0xFF;
/* Host keyboard LED bitmap from 5A frames (USB LED bits; bit1 = caps lock). */
static volatile uint8_t host_leds = 0;

/* Pending connection request: the profile the user asked for, retried until the
 * module reports connected (or the request is cleared by leaving BT mode). */
static volatile bool    connect_requested = false;
static volatile uint8_t requested_profile = 0;
/* True while the mode slider is in the USB position (wireless link not in use).
 * Set from the dip-switch handler via ch582_cancel_connect / ch582_set_profile. */
static volatile bool    usb_mode = false;
/* True once the module has ACKed anything, i.e. its UART is up and receiving.
 * Cold boot is the only time it is false (the boot-time slot select can fire
 * before the CH582F finishes powering up). Used to bound the select retry: we
 * re-issue the select only until the module is alive, NOT until it connects --
 * re-selecting a live module restarts its advertising and starves slow (macOS)
 * reconnects, which need the peripheral to keep advertising uninterrupted. */
static volatile bool    module_alive = false;
static uint16_t         last_attempt_time = 0;
/* Last time a battery poll (A6 53) was sent. */
static uint16_t         last_battery_poll = 0;
/* When conn_state last became LINKING; dwell base for the 5A promote backstop. */
static uint16_t         linking_since = 0;

/* A6 51 (pairing) resend state: pending until the module confirms via 5B 31/32
 * or the retry budget runs out. */
static volatile bool    pairing_pending = false;
static uint8_t          pairing_tries      = 0;
static uint16_t         pairing_last_send  = 0;

/* Same-slot-reselect-while-advertising bounce state (see CH582_BOUNCE_MS). */
static volatile bool    bounce_pending        = false;
static uint8_t          bounce_target_profile = 0;
static uint16_t         bounce_started        = 0;

/* Request a battery-level report from the module. Logic-analyzer-decoded from the
 * stock firmware: the MCU sends `A6 53` and the module replies with a `5C <pct>`
 * frame (parsed in ch582_task). The module is otherwise silent, so battery only
 * updates if we poll. */
#define CH582_BATTERY_REQ_PARAM 0x53
void ch582_poll_status(void) {
    uint8_t param = CH582_BATTERY_REQ_PARAM;
    ch582_send_command(0xA6, &param, 1);
}

typedef enum {
    STATE_WAIT_TYPE = 0,
    STATE_WAIT_DATA,
    STATE_WAIT_CHECKSUM
} parse_state_t;

static const SerialConfig serial_cfg = {
    .speed               = 115200,
    .UART_WordLength     = 3,  // 8 data bits
    .UART_StopBits       = 0,  // 1 stop bit
    .UART_Parity         = 0,  // No parity
    .UART_FIFOControl    = 0,
    .UART_AutoBaudControl = 0,
    .UART_Oversampling   = 16,
    .UART_HalfDuplexMode = 0,
};

/* True when key reports should be forwarded to the module over UART: a BT/2.4G
 * profile is selected (dip switch in a wireless position). Deliberately NOT
 * gated on is_module_connected: bluetooth_send_keyboard() never was (it always
 * sends A1 and relies on the reliable TX queue to hold/retry), and gating only
 * the consumer path on link state made the encoder go silent whenever the link
 * was momentarily down (e.g. stranded in LINKING) while typing kept working --
 * an asymmetry that read as "Bluetooth mostly works". In USB mode
 * connect_requested is cleared, so we never double-type or double-send. */
static bool ch582_kbd_output_active(void) {
    return connect_requested;
}

/* Service the CH582 link once: pump the reliable TX queue and process any
 * available RX (ACK -> release in-flight frame, connection state, battery).
 * Factored out of ch582_task() so the keyboard send path can also drain the
 * queue mid-burst (see the backpressure in ch582_send_keyboard_report). */
static void ch582_service(void);
/* True when the reliable TX queue has no free slot. */
static bool ch582_txq_full(void);

/* Max time to spend draining the TX queue for a single keyboard report before
 * giving up (and dropping, as before). Only reached on a stalled link; a healthy
 * link frees a slot in ~1-2 ms. */
#ifndef CH582_TX_BACKPRESSURE_MS
#    define CH582_TX_BACKPRESSURE_MS 50
#endif

void ch582_send_keyboard_report(report_keyboard_t *report) {
    /* Backpressure: a VIA macro / fast burst enqueues key reports far faster than
     * the ACK-gated BT link drains, and ch582_task() cannot run mid-macro (macro
     * playback blocks the main loop), so the fixed-depth queue would overflow and
     * silently drop the tail. While the link is up, service it here until a slot
     * frees, so the burst paces to the link instead of losing frames. Bounded, so a
     * stalled/absent link degrades to the old drop-on-full behaviour rather than
     * wedging the keyboard. */
    if (is_module_connected) {
        uint16_t start = timer_read();
        while (ch582_txq_full() && timer_elapsed(start) < CH582_TX_BACKPRESSURE_MS) {
            ch582_service();
        }
    }

    /* Boot keyboard report = [mods][reserved][key1..key6]; this matches the
     * stock A1 frame byte-for-byte. report->keys is the 6KRO slot array, valid
     * regardless of NKRO (it is the first member of the report union). */
    uint8_t buf[8];
    buf[0] = report->mods;
    buf[1] = 0; /* reserved */
    for (uint8_t i = 0; i < 6; i++) {
        buf[2 + i] = report->keys[i];
    }
    ch582_send_command(0xA1, buf, sizeof(buf));
}

/* --- QMK Bluetooth driver API ------------------------------------------------
 * With BLUETOOTH_DRIVER = custom, QMK's host.c routes key reports to bt_driver
 * (these bluetooth_* functions) whenever the active connection host is
 * Bluetooth, and to the USB driver otherwise. The mode slider drives the active
 * host (see dip_switch_update_user). No host_set_driver hacks, no clobber, and
 * no manual USB/wireless routing -- core handles it. */
void bluetooth_init(void) {
    is_module_connected = false;
    sdStart(&CH582_SERIAL_DRIVER, &serial_cfg);
}

void bluetooth_task(void) {
    ch582_task();
}

bool bluetooth_is_connected(void) {
    return is_module_connected;
}

/* The module's wireless input is a 6KRO boot report (A1). Reporting NKRO as
 * unsupported makes host_can_send_nkro() return false while Bluetooth is the
 * active host, so QMK sends a plain boot report_keyboard_t via
 * bluetooth_send_keyboard() -- no NKRO->boot conversion needed here. USB keeps
 * NKRO (keyboard.json "nkro": true) when USB is the active host. */
bool bluetooth_can_send_nkro(void) {
    return false;
}

uint8_t bluetooth_keyboard_leds(void) {
    return host_leds;
}

void bluetooth_send_keyboard(report_keyboard_t *report) {
    ch582_send_keyboard_report(report);
}

void bluetooth_send_nkro(report_nkro_t *report) {
    /* Not reached: bluetooth_can_send_nkro() is false. Stubbed for safety. */
}

void bluetooth_send_mouse(report_mouse_t *report) {}

/* Consumer control (volume/media, e.g. the encoder) over the wireless link.
 * The encoder emits HID consumer usages; without forwarding them here they only
 * work over USB. Frame is 0xA3 + 2-byte little-endian usage, mirroring the A1
 * keyboard framing -- confirmed working on hardware over both BT and 2.4G. */
void bluetooth_send_consumer(uint16_t usage) {
    if (!ch582_kbd_output_active()) return;
    uint8_t buf[2];
    buf[0] = usage & 0xFF;
    buf[1] = (usage >> 8) & 0xFF;
    ch582_send_command(0xA3, buf, sizeof(buf));
}

void bluetooth_send_system(uint16_t usage) {}

/* --- reliable (ACK'd, retrying) TX queue --------------------------------------
 * The module ACKs every frame we send with `61 0D 0A`. Sending once and ignoring
 * that ACK means a dropped/checksum-rejected frame is simply lost -- for a key
 * RELEASE that strands the key and the host auto-repeats it (the sporadic BT
 * stuck-key). Stock (and the open @isuua reference in aliou/keebs,
 * vendor/edthu-wireless -- the same module protocol) instead queue each frame and
 * retransmit until the module ACKs. This is a minimal port of that: one frame
 * in flight at a time, retried on ACK-timeout, popped on ACK. Frames are
 * idempotent (they carry STATE, not events), so a retransmit is always safe. */
#define CH582_TXQ_LEN       24
#define CH582_TX_FRAME_MAX  (TX_MAX_PAYLOAD + 2)
#ifndef CH582_TX_ACK_TIMEOUT_MS
#    define CH582_TX_ACK_TIMEOUT_MS 10
#endif
#ifndef CH582_TX_MAX_RETRIES
#    define CH582_TX_MAX_RETRIES 8
#endif

typedef struct {
    uint8_t data[CH582_TX_FRAME_MAX];
    uint8_t len;
} ch582_tx_frame_t;

static ch582_tx_frame_t tx_q[CH582_TXQ_LEN];
static uint8_t          tx_head = 0, tx_tail = 0;   /* head = frame in flight / next to send */
static bool             tx_in_flight = false;
static uint16_t         tx_sent_time = 0;
static uint8_t          tx_retries   = 0;

static inline uint8_t tx_next(uint8_t i) { return (uint8_t)((i + 1) % CH582_TXQ_LEN); }

static bool ch582_txq_full(void) { return tx_next(tx_tail) == tx_head; }

/* Drop the head frame (ACKed or given up) and stop waiting on it. */
static void ch582_tx_pop(void) {
    if (tx_head != tx_tail) tx_head = tx_next(tx_head);
    tx_in_flight = false;
    tx_retries   = 0;
}

/* Advance the queue: send the head frame, time out and retransmit, or give up.
 * Called every ch582_task() (main-loop cadence, sub-ms), so the 10 ms timeout is
 * honoured with fine granularity. */
static void ch582_tx_pump(void) {
    if (tx_in_flight) {
        if (timer_elapsed(tx_sent_time) < CH582_TX_ACK_TIMEOUT_MS) return;   /* still waiting for ACK */
        if (++tx_retries > CH582_TX_MAX_RETRIES) { ch582_tx_pop(); return; } /* give up, move on */
        sdWrite(&CH582_SERIAL_DRIVER, tx_q[tx_head].data, tx_q[tx_head].len); /* retransmit */
        tx_sent_time = timer_read();
        return;
    }
    if (tx_head == tx_tail) return;                                          /* queue empty */
    sdWrite(&CH582_SERIAL_DRIVER, tx_q[tx_head].data, tx_q[tx_head].len);
    tx_sent_time = timer_read();
    tx_in_flight = true;
}

/* The module ACKed the in-flight frame -> drop it and let the next one go. Also
 * the first proof the module's UART is up (see module_alive). */
static void ch582_tx_ack(void) {
    module_alive = true;
    if (tx_in_flight) ch582_tx_pop();
}

void ch582_send_command(uint8_t cmd, const uint8_t *params, uint8_t param_len) {
    if (param_len > TX_MAX_PAYLOAD) return;

    uint8_t nxt = tx_next(tx_tail);
    if (nxt == tx_head) return;                 /* queue full -> drop (should not happen at typing rates) */

    ch582_tx_frame_t *f = &tx_q[tx_tail];
    uint16_t sum = cmd;
    f->data[0]   = cmd;
    for (uint8_t i = 0; i < param_len; i++) {
        f->data[1 + i] = params[i];
        sum += params[i];
    }
    f->data[param_len + 1] = (uint8_t)(sum & 0xFF);
    f->len                 = param_len + 2;
    tx_tail                = nxt;
}

#if CH582_ACK_FRAMES
/* Raw 61 0D 0A ACK token (not a checksummed command, so it bypasses
 * ch582_send_command). Stock replies with this to 5B/5C frames. */
static void ch582_send_ack(void) {
    static const uint8_t ack[3] = {0x61, 0x0D, 0x0A};
    sdWrite(&CH582_SERIAL_DRIVER, ack, sizeof(ack));
}
#endif

void ch582_set_profile(ch582_profile_t profile) {
    uint8_t param = (uint8_t)profile;

    /* Reselecting the slot that's currently advertising is otherwise a no-op
     * for the module (see CH582_BOUNCE_MS above): bounce through a different
     * slot first, then let ch582_task() re-issue the real target after the
     * dwell instead of sending both back-to-back. */
    if (is_pairing && requested_profile == param) {
        uint8_t bounce         = (param == CH582_PROFILE_BT_1) ? (uint8_t)CH582_PROFILE_BT_2 : (uint8_t)CH582_PROFILE_BT_1;
        bounce_pending         = true;
        bounce_target_profile  = param;
        bounce_started         = timer_read();
        ch582_send_command(0xA6, &bounce, 1);
    } else {
        bounce_pending = false;   /* a fresh selection supersedes any bounce in flight */
        ch582_send_command(0xA6, &param, 1);
    }

    pairing_pending      = false;   /* a new selection supersedes any pairing retry in flight */
    requested_profile    = param;
    connect_requested    = true;
    usb_mode             = false;
    is_module_connected  = false;
    conn_state           = CH582_CONN_LINKING;   /* attempting until 5B says otherwise */
    linking_since        = timer_read();
    last_attempt_time    = timer_read();
}

/* Pairing command, decoded from stock TX captures: a CONSTANT `0xA6 0x51`, sent
 * twice, which puts the CURRENTLY-SELECTED slot into pairing/advertising. It
 * carries no slot of its own (slot 2 and slot 3 pairing BOTH emitted `A6 51`), so
 * the target slot must already be selected via ch582_set_profile() first. This is
 * why the stock long-press only pairs the active slot. The old 0xA9/0xA1 guess was
 * wrong (0xA1 is the keystroke opcode). */
#define CH582_PAIR_PARAM 0x51

void ch582_enter_pairing(void) {
    uint8_t param       = CH582_PAIR_PARAM;
    is_module_connected = false;
    is_pairing          = true;
    conn_state          = CH582_CONN_PAIRING;
    /* A6 51 carries no slot of its own and the link has no ACK, so a dropped
     * frame here silently strands hold-to-pair (see CH582_PAIR_RETRY_MS
     * above). Resend from ch582_task() until 5B 31/32 confirms or we give up. */
    pairing_pending     = true;
    pairing_tries       = 0;
    pairing_last_send   = timer_read();
    ch582_send_command(0xA6, &param, 1);
}

void ch582_pair(ch582_profile_t profile) {
    /* Select the slot, then put it into pairing. (On stock, select is the short
     * press and pairing the long press on the same slot; this convenience helper
     * does both for callers that just want "pair this slot now".) */
    ch582_set_profile(profile);
    ch582_enter_pairing();
}

void ch582_cancel_connect(void) {
    connect_requested = false;
    usb_mode          = true;
    conn_state        = CH582_CONN_IDLE;
    pairing_pending   = false;
    bounce_pending    = false;
}

bool ch582_is_connected(void) {
    return is_module_connected;
}

uint8_t ch582_get_slot(void) {
    return connected_slot;
}

bool ch582_is_24g(void) {
    return !usb_mode && requested_profile == CH582_PROFILE_PEER_24G;
}

bool ch582_is_usb(void) {
    return usb_mode;
}

bool ch582_is_pairing(void) {
    return is_pairing;
}

uint8_t ch582_get_battery(void) {
    return battery_level;
}

uint8_t ch582_get_host_leds(void) {
    return host_leds;
}

/* Map a selected 0xA6 profile byte (0x31..0x33) to a 1..3 slot; 0 for 2.4G/none. */
static uint8_t profile_to_slot(uint8_t profile) {
    if (profile >= CH582_PROFILE_BT_1 && profile <= CH582_PROFILE_BT_3) {
        return profile - 0x30;
    }
    return 0;
}

ch582_conn_state_t ch582_get_conn_state(void) {
    return conn_state;
}

/* Slot to display: the slot we are aiming for, valid while linking/pairing (not
 * just when connected). 2.4G shows as slot 1; USB/none is 0. */
uint8_t ch582_get_target_slot(void) {
    if (usb_mode) return 0;
    if (requested_profile == CH582_PROFILE_PEER_24G) return 1;
    return profile_to_slot(requested_profile);
}

/* Rolling 3-byte window over the RX stream, shared by ch582_service() across
 * calls (ch582_task and the send-path backpressure both drive it). The module
 * interleaves two frame formats with no length prefix, and the stream can drop
 * bytes on a burst, so a fixed [type,data,cksum] state machine desyncs
 * permanently. Matching on a sliding window self-resynchronises and tolerates
 * dropped bytes.
 *
 * Logic-analyzer-decoded against stock firmware. The ONLY trustworthy connection
 * signals are the 5B state transitions; everything else (61 0D 0A, 5C battery,
 * 5B 23) is periodic and streams regardless of link state:
 *   - 61 0D 0A   -> "a\r\n" periodic IDLE HEARTBEAT (NOT a disconnect: it is
 *                   emitted while connected too). Ignore for connection state.
 *   - 5A <led>   -> host keyboard LED bitmap (USB LED bits; bit1 = caps lock)
 *   - 5B <code>  -> connection state machine (code is a STATE, not a slot):
 *        32 = link established (connected);  31 = advertising/pairing;
 *        33/34 = connect ATTEMPT (link down, retrying);  23 = idle (ignore,
 *        it appears both connected and disconnected).
 *   - 5C <pct>   -> battery percent in decimal (0x64=100). PERIODIC and link-
 *                   independent (streams even while disconnected) -> never use
 *                   it as a "connected" signal. */
static uint8_t b2 = 0, b1 = 0, b0 = 0;

static void ch582_service(void) {
    /* Drive the reliable TX queue: send/retransmit/drop the in-flight frame. */
    ch582_tx_pump();

    uint8_t c;
    uint8_t bytes_processed = 0;

    /* Drain the queue fully (capped to bound worst-case latency) so a connect
     * burst doesn't overflow the serial input buffer and shed bytes. */
    while (bytes_processed < 64) {
        if (chnReadTimeout(&CH582_SERIAL_DRIVER, &c, 1, TIME_IMMEDIATE) == 0) break;
        bytes_processed++;

        b2 = b1;
        b1 = b0;
        b0 = c;

        bool matched = false;

        if (b2 == 0x61 && b1 == 0x0D && b0 == 0x0A) {
            /* `61 0D 0A` ("a\r\n") is the per-frame ACK the module returns for what
             * we send -- NOT a connection signal (it is silent at idle). Use it to
             * release the in-flight TX frame so the next queued one can go. */
            ch582_tx_ack();
            matched             = true;
        } else if ((b2 == 0x5A || b2 == 0x5B || b2 == 0x5C) &&
                   b0 == ((uint8_t)(b2 + b1))) {
            uint8_t type       = b2;
            uint8_t d          = b1;
            switch (type) {
                case 0x5A: /* host LED bitmap (caps lock = bit1) */
                    /* The 1-byte additive checksum is weak, so the mixed RX stream
                     * (notably the CH582F power-up / 2.4G link-up burst) can throw a
                     * bogus 5A frame that spuriously lights Caps. Guard twofold:
                     *   - only honor LED frames once a link exists (or is about to be
                     *     inferred as one below), and
                     *   - require a plausible HID LED bitmap (low 5 bits only:
                     *     num/caps/scroll/compose/kana). Random garbage that happens
                     *     to pass the checksum usually has high bits set. */
                    if ((d & ~0x1F) == 0) {
                        if (is_module_connected) {
                            host_leds = d;
                        } else if (conn_state == CH582_CONN_LINKING &&
                                   timer_elapsed(linking_since) >= CH582_5A_PROMOTE_MS) {
                            /* Backstop for a dropped 5B 32 (see CH582_5A_PROMOTE_MS
                             * above): this frame is a side effect of an established
                             * link, so treat it as one. */
                            is_module_connected = true;
                            is_pairing          = false;
                            conn_state          = CH582_CONN_CONNECTED;
                            connected_slot      = profile_to_slot(requested_profile);
                            pairing_pending      = false;
                            host_leds            = d;
                        }
                    }
                    break;
                case 0x5B: /* connection state code (NOT a slot number) */
                    switch (d) {
                        case 0x32: /* link established - the only "connected" signal */
                            is_module_connected = true;
                            is_pairing          = false;
                            conn_state          = CH582_CONN_CONNECTED;
                            connected_slot      = profile_to_slot(requested_profile);
                            pairing_pending      = false;
                            break;
                        case 0x31: /* advertising / pairing, waiting for a device */
                            is_pairing          = true;
                            is_module_connected = false;
                            conn_state          = CH582_CONN_PAIRING;
                            connected_slot      = 0;
                            host_leds           = 0; /* no link -> drop stale LED state */
                            pairing_pending      = false;   /* module confirmed it heard the pair request */
                            break;
                        case 0x33: /* connect ATTEMPT - link is down and retrying */
                        case 0x34:
                            is_module_connected = false;
                            is_pairing          = false;
                            conn_state          = CH582_CONN_LINKING;
                            connected_slot      = 0;
                            host_leds           = 0; /* no link -> drop stale LED state */
                            linking_since        = timer_read();
                            break;
                        case 0x36: /* host REFUSED the connection (edthu MD_REV REJECT) */
                            is_module_connected = false;
                            is_pairing          = false;
                            conn_state          = CH582_CONN_REJECTED;
                            connected_slot      = 0;
                            host_leds           = 0;
                            break;
                        default:   /* 0x23 idle/finalize: periodic, appears both
                                    * connected and disconnected -> leave state */
                            break;
                    }
                    break;
                case 0x5C: /* battery percent; PERIODIC and link-independent (streams
                            * even while disconnected) -> do NOT touch connection state */
                    if (d <= 100) battery_level = d;
                    break;
            }
#if CH582_ACK_FRAMES
            /* Mirror stock: ack 5B (connection state) and 5C (battery), never 5A. */
            if (type == 0x5B || type == 0x5C) ch582_send_ack();
#endif
            matched = true;
        }

        if (matched) {
            /* Clear the window so the consumed bytes can't form a phantom overlap
             * match with the next byte. */
            b2 = b1 = b0 = 0;
        }
    }
}

void ch582_task(void) {
    /* Cold-boot-only select retry. The mode slider is wired directly to the
     * CH582F, so once the module is up it handles advertising/reconnect itself
     * (like the @isuua/edthu reference, which sends the select ONCE per switch).
     * We must NOT keep re-issuing the select on a live module: re-selecting
     * restarts its advertising and starves slow reconnects -- a phone reconnects
     * in <500 ms and beats the retry, but macOS directed-advertising takes longer
     * and never completes, so it needed a manual pairing entry. The only case
     * that genuinely needs a retry is cold boot, where the boot-time select fires
     * before the CH582F's UART is up and is dropped. So retry ONLY until the
     * module is alive (has ACKed anything), then stop and let it reconnect at its
     * own pace. Transient (non-boot) select drops are covered by the ACK/retry
     * queue instead. */
    if (connect_requested && !module_alive &&
        timer_elapsed(last_attempt_time) >= CH582_CONNECT_RETRY_MS) {
        last_attempt_time = timer_read();
        uint8_t param = requested_profile;
        ch582_send_command(0xA6, &param, 1);
    }

    /* Resend A6 51 (pairing) until the module confirms via 5B 31/32, since a
     * dropped frame here has no other symptom to catch it (see
     * CH582_PAIR_RETRY_MS above). */
    if (pairing_pending && timer_elapsed(pairing_last_send) >= CH582_PAIR_RETRY_MS) {
        if (++pairing_tries > CH582_PAIR_MAX_TRIES) {
            pairing_pending = false;
        } else {
            pairing_last_send = timer_read();
            uint8_t param      = CH582_PAIR_PARAM;
            ch582_send_command(0xA6, &param, 1);
        }
    }

    /* Follow through on a same-slot-reselect bounce (see CH582_BOUNCE_MS
     * above): the dwell has passed, so re-issue the real target now. */
    if (bounce_pending && timer_elapsed(bounce_started) >= CH582_BOUNCE_MS) {
        bounce_pending = false;
        uint8_t param   = bounce_target_profile;
        ch582_send_command(0xA6, &param, 1);
    }

    /* Periodically poll the module for battery level. The module only emits 5C
     * battery frames in response to an A6 53 request (the stock firmware polls the
     * same way). Poll regardless of connection mode: the gauge is valid in BT,
     * 2.4G and USB-charging states, and A6 53 is a status query that doesn't touch
     * the link. */
    if (timer_elapsed(last_battery_poll) >= CH582_BATTERY_POLL_MS) {
        last_battery_poll = timer_read();
        ch582_poll_status();
    }

    /* Pump the TX queue and drain RX (ACKs, connection state, battery). Same
     * routine the send-path backpressure calls to keep the queue moving mid-burst. */
    ch582_service();
}
