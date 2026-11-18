// Force feedback: console (Logitech G29 "classic" 7-byte commands) -> Simagic
// wheelbase (new firmware, >= v159), pure C state machine.
//
// Console side (facts from jfedor2/wheel-adapter and bilgon32/ps5-wheel-adapter,
// author-reported GT7/PS5): commands arrive in output report 5. byte0 = slot mask
// (0x10 F1 .. 0x80 F4) | operation (0 download, 1 download+play, 2 play, 3 stop,
// 0xC refresh, ...). byte1 = force type (0x00 constant: level per slot in bytes
// 2..5; 0x08 variable: L1 byte 2 -> F1, L2 byte 3 -> F3). Levels 0..255, 127 and
// 128 = no force. 0xF8 = extended commands (range, LEDs, mode), never forwarded.
//
// Wheelbase side (protocol facts from JacKeTUs/simagic-ff, GPL-2.0, reimplemented):
// every command is the device's first output report, one byte per value:
//   01 set effect   [01, block, 01, dur_lo, dur_hi, 0,0,0,0, FF, FF, 04, 3F]
//   05 constant     [05, block, mag_lo, mag_hi]   mag: signed -10000..10000
//   0A operation    [0A, block, 01 start | 03 stop, loops, 00]
//
// Only steady (constant / zero-step variable) forces are translated. Spring,
// damper, friction and periodic types are counted and ignored until a capture
// shows GT7 uses them. Every magnitude is scaled by strength_percent (the cap) and
// clamped, so no console command can exceed it.
//
// Like a real Logitech wheel, a force is held until the console changes or stops
// it: GT7 sends commands only on change (measured gaps > 2 s while driving), so
// there is no traffic-based timeout.
#ifndef FFB_H
#define FFB_H

#include <stdbool.h>
#include <stdint.h>

#define FFB_CMD_LEN 7
#define FFB_OUT_MAX 63  // bytes after the report ID

#define SM_SET_EFFECT 0x01
#define SM_SET_CONSTANT 0x05
#define SM_EFFECT_OP 0x0A
#define SM_OP_START 0x01
#define SM_OP_STOP 0x03
#define SM_BLOCK_CONSTANT 0x01

typedef enum {
    FFB_EV_FORMAT = 1,       // a: ok, b: report id | out len << 8
    FFB_EV_FIRST_CMD,        // a: byte0, b: byte1 | byte2 << 8 | byte3 << 16  (first of each op/type pair)
    FFB_EV_UNSUPPORTED,      // a: force type, b: count of that type
    FFB_EV_EXTENDED,         // a: byte1 of an 0xF8/0xFx command, b: count
    FFB_EV_ACTIVE,           // a: 1 started / 0 stopped, b: magnitude sent (int16 as uint)
    FFB_EV_RESET,            // a: reason (ffb_reset_t)
    FFB_EV_STALE,            // unused since 0.2.4 (no traffic timeout); kept for log compatibility
    FFB_EV_OUT_MODE,         // a: 1 interrupt OUT, 2 control SET_REPORT
} ffb_event_t;

typedef enum {
    FFB_RESET_BOOT = 1,
    FFB_RESET_CONSOLE_GONE = 2,
    FFB_RESET_SUSPEND = 3,
    FFB_RESET_WHEEL_GONE = 4,
    FFB_RESET_STALE = 5,     // unused since 0.2.4
    FFB_RESET_TEST = 6,
} ffb_reset_t;

typedef void (*ffb_event_cb_t)(void* ctx, ffb_event_t ev, uint8_t a, uint32_t b);

typedef struct {
    bool playing;
    uint8_t type;
    uint8_t level;  // 0..255, 0x80 = none
} ffb_slot_t;

typedef struct {
    // configuration
    uint8_t strength_percent;  // cap, 0..100
    bool invert;
    ffb_event_cb_t ev;
    void* ev_ctx;

    // wheelbase output report
    bool out_ok;
    uint8_t out_report_id;
    uint8_t out_len;  // value bytes after the report ID

    // console state
    ffb_slot_t slot[4];
    uint32_t last_cmd_ms;
    bool any_cmd;

    // test override (bench only): magnitude in -10000..10000 before the cap, 0 = off
    bool test_active;
    int16_t test_mag;
    uint32_t test_until_ms;

    // what the wheelbase has been sent
    uint8_t stops_pending;  // boot/reset: blocks still to stop
    bool uploaded;
    bool running;
    int16_t sent_mag;
    bool mag_valid;  // sent_mag is known to be on the wheelbase
    bool packet_outstanding;
    uint8_t outstanding_kind;
    int16_t pending_mag;
    uint8_t pending_stop_bit;

    // statistics
    uint32_t rx_cmds, rx_bad, ext_cmds, unsupported, tx_packets, tx_failed;
    uint32_t max_gap_ms;
    uint16_t seen_pairs[16];  // bit per force type, per operation, for first-seen logging
    uint16_t unsupported_by_type[16];
} ffb_t;

void ffb_init(ffb_t* f, uint8_t strength_percent, bool invert, ffb_event_cb_t ev, void* ctx);

// Finds the wheelbase's first output report (as simagic-ff does) and checks it can
// carry the 13-byte set-effect packet with 8-bit values. Disables FFB otherwise.
bool ffb_set_output_format(ffb_t* f, const uint8_t* desc, uint16_t len);

// One console output report as delivered by TinyUSB's tud_hid_set_report_cb.
void ffb_console_output(ffb_t* f, uint8_t report_id, const uint8_t* buf, uint16_t len, uint32_t now_ms);

// Clears all forces (console gone, suspend, wheelbase gone, boot) and queues stops.
void ffb_reset(ffb_t* f, ffb_reset_t reason);

// Bench self-test: apply mag (-10000..10000, before the cap) for duration_ms.
void ffb_test(ffb_t* f, int16_t mag, uint32_t duration_ms, uint32_t now_ms);

// Signed console force in -127..127 from the playing steady slots (after invert).
int16_t ffb_console_force(const ffb_t* f);
// Magnitude to send now (-10000..10000, capped), 0 if nothing should play.
int16_t ffb_target_mag(const ffb_t* f, uint32_t now_ms);

// Builds the next wheelbase packet (value bytes, report ID excluded, zero-padded to
// out_len). Returns 0 when nothing needs sending. Call ffb_packet_done() after.
uint8_t ffb_next_packet(ffb_t* f, uint32_t now_ms, uint8_t* out);
void ffb_packet_done(ffb_t* f, bool ok);

#endif
