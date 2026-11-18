// Force feedback translation. See ffb.h.
#include "ffb.h"

#include <string.h>

#include "hid_desc.h"

#define TYPE_CONSTANT 0x00
#define TYPE_VARIABLE 0x08

enum { KIND_NONE = 0, KIND_STOP, KIND_UPLOAD, KIND_CONST, KIND_START };

// Blocks stopped at boot so nothing left over from another host keeps playing:
// constant, sine, damper, spring, friction, inertia (simagic-ff block ids).
static const uint8_t stop_blocks[] = { 0x01, 0x02, 0x05, 0x06, 0x07, 0x09 };
#define STOP_ALL ((uint8_t) ((1u << sizeof(stop_blocks)) - 1u))
#define STOP_CONSTANT 0x01u

static void emit(ffb_t* f, ffb_event_t ev, uint8_t a, uint32_t b) {
    if (f->ev) {
        f->ev(f->ev_ctx, ev, a, b);
    }
}

void ffb_init(ffb_t* f, uint8_t strength_percent, bool invert, ffb_event_cb_t ev, void* ctx) {
    memset(f, 0, sizeof(*f));
    f->strength_percent = strength_percent > 100 ? 100 : strength_percent;
    f->invert = invert;
    f->ev = ev;
    f->ev_ctx = ctx;
    for (int i = 0; i < 4; i++) {
        f->slot[i].level = 0x80;
    }
}

// ---------------------------------------------------------------- output format

typedef struct {
    bool found;
    uint8_t id;
    uint16_t run_bytes;  // contiguous 8-bit values from bit 0
    bool broken;
} out_ctx_t;

static void out_cb(void* vctx, const hid_element_t* el) {
    out_ctx_t* c = (out_ctx_t*) vctx;
    if (el->type != HID_RPT_OUTPUT) {
        return;
    }
    if (!c->found) {
        c->found = true;
        c->id = el->report_id;
    }
    if (el->report_id != c->id || c->broken) {
        return;
    }
    if (el->bit_size == 8 && el->bit_offset == c->run_bytes * 8u) {
        c->run_bytes++;
    } else {
        c->broken = true;
    }
}

bool ffb_set_output_format(ffb_t* f, const uint8_t* desc, uint16_t len) {
    out_ctx_t c;
    memset(&c, 0, sizeof(c));
    hid_desc_walk(desc, len, out_cb, &c);
    uint32_t bits = c.found ? hid_desc_report_bits(desc, len, HID_RPT_OUTPUT, c.id) : 0;
    f->out_ok = c.found && c.run_bytes >= 13 && bits / 8 <= FFB_OUT_MAX && bits % 8 == 0;
    f->out_report_id = c.id;
    f->out_len = (uint8_t) (bits / 8 > FFB_OUT_MAX ? FFB_OUT_MAX : bits / 8);
    emit(f, FFB_EV_FORMAT, f->out_ok, (uint32_t) c.id | ((uint32_t) f->out_len << 8) | ((uint32_t) c.run_bytes << 16));
    return f->out_ok;
}

// ---------------------------------------------------------------- console side

static bool steady(uint8_t type) {
    return type == TYPE_CONSTANT || type == TYPE_VARIABLE;
}

static int16_t level_signed(uint8_t level) {
    if (level < 127) {
        return (int16_t) -(127 - level);
    }
    if (level > 128) {
        return (int16_t) (level - 128);
    }
    return 0;  // 127 and 128 both mean no force
}

static uint8_t slot_level(const uint8_t* cmd, uint8_t type, int slot) {
    if (type == TYPE_CONSTANT) {
        return cmd[2 + slot];
    }
    if (type == TYPE_VARIABLE) {
        // Two initial levels: L1 -> F1, L2 -> F3. Non-zero step/time nibbles (ramps the
        // wheel would generate itself) are approximated by their initial level.
        return slot == 0 ? cmd[2] : slot == 2 ? cmd[3] : 0x80;
    }
    return 0x80;
}

void ffb_console_output(ffb_t* f, uint8_t report_id, const uint8_t* buf, uint16_t len, uint32_t now_ms) {
    const uint8_t* cmd;
    if (report_id == 0 && len >= 1 + FFB_CMD_LEN) {
        cmd = buf + 1;  // interrupt OUT: report ID byte first (jfedor2 uses buffer + 1)
    } else if (report_id == 5 && len >= FFB_CMD_LEN) {
        cmd = buf;  // SET_REPORT(output, 5): ID already stripped
    } else {
        f->rx_bad++;
        return;
    }
    f->rx_cmds++;
    if (f->any_cmd && (uint32_t) (now_ms - f->last_cmd_ms) > f->max_gap_ms) {
        f->max_gap_ms = now_ms - f->last_cmd_ms;
    }
    f->last_cmd_ms = now_ms;
    f->any_cmd = true;

    uint8_t op = cmd[0] & 0x0F;
    uint8_t slots = cmd[0] >> 4;
    uint8_t type = cmd[1];

    uint16_t bit = (uint16_t) (1u << (type & 0x0F));
    if (!(f->seen_pairs[op] & bit)) {
        f->seen_pairs[op] |= bit;
        emit(f, FFB_EV_FIRST_CMD, cmd[0], (uint32_t) cmd[1] | ((uint32_t) cmd[2] << 8) | ((uint32_t) cmd[3] << 16) |
                                             ((uint32_t) cmd[4] << 24));
    }

    switch (op) {
        case 0x0:  // download
        case 0x1:  // download and play
        case 0xC:  // refresh
            if (!steady(type)) {
                f->unsupported++;
                uint16_t n = ++f->unsupported_by_type[type & 0x0F];
                if (n == 1 || (n & 0x3FF) == 0) {
                    emit(f, FFB_EV_UNSUPPORTED, type, n);
                }
            }
            for (int s = 0; s < 4; s++) {
                if (slots & (1u << s)) {
                    f->slot[s].type = type;
                    f->slot[s].level = slot_level(cmd, type, s);
                    if (op == 0x1) {
                        f->slot[s].playing = true;
                    }
                }
            }
            break;
        case 0x2:  // play
        case 0x3:  // stop
            for (int s = 0; s < 4; s++) {
                if (slots & (1u << s)) {
                    f->slot[s].playing = (op == 0x2);
                }
            }
            break;
        default:  // 0x8 extended (range, LEDs, mode), default spring, dead band, loops ...
            f->ext_cmds++;
            if (f->ext_cmds == 1 || (f->ext_cmds & 0x3FF) == 0) {
                emit(f, FFB_EV_EXTENDED, cmd[1], f->ext_cmds);
            }
            break;
    }
}

int16_t ffb_console_force(const ffb_t* f) {
    int32_t sum = 0;
    for (int s = 0; s < 4; s++) {
        if (f->slot[s].playing && steady(f->slot[s].type)) {
            sum += level_signed(f->slot[s].level);
        }
    }
    if (sum > 127) sum = 127;
    if (sum < -127) sum = -127;
    return (int16_t) (f->invert ? -sum : sum);
}

static bool console_wants_run(const ffb_t* f) {
    for (int s = 0; s < 4; s++) {
        if (f->slot[s].playing && steady(f->slot[s].type)) {
            return true;
        }
    }
    return false;
}

static bool test_running(const ffb_t* f, uint32_t now_ms) {
    return f->test_active && (int32_t) (f->test_until_ms - now_ms) > 0;
}

int16_t ffb_target_mag(const ffb_t* f, uint32_t now_ms) {
    int32_t mag;
    if (test_running(f, now_ms)) {
        mag = (int32_t) f->test_mag * f->strength_percent / 100;
    } else {
        mag = (int32_t) ffb_console_force(f) * 10000 * f->strength_percent / (127 * 100);
    }
    int32_t cap = 100 * (int32_t) f->strength_percent;
    if (mag > cap) mag = cap;
    if (mag < -cap) mag = -cap;
    return (int16_t) mag;
}

void ffb_reset(ffb_t* f, ffb_reset_t reason) {
    for (int s = 0; s < 4; s++) {
        f->slot[s].playing = false;
        f->slot[s].level = 0x80;
    }
    f->test_active = false;
    if (reason == FFB_RESET_BOOT || reason == FFB_RESET_WHEEL_GONE) {
        // New or re-enumerated wheelbase: its effect state is unknown.
        f->stops_pending = STOP_ALL;
        f->uploaded = false;
        f->running = false;
        f->mag_valid = false;
        f->packet_outstanding = false;
    } else {
        f->stops_pending |= STOP_CONSTANT;
    }
    emit(f, FFB_EV_RESET, (uint8_t) reason, 0);
}

void ffb_test(ffb_t* f, int16_t mag, uint32_t duration_ms, uint32_t now_ms) {
    if (mag > 10000) mag = 10000;
    if (mag < -10000) mag = -10000;
    f->test_active = true;
    f->test_mag = mag;
    f->test_until_ms = now_ms + duration_ms;
}

// ---------------------------------------------------------------- wheelbase side

uint8_t ffb_next_packet(ffb_t* f, uint32_t now_ms, uint8_t* out) {
    if (!f->out_ok || f->packet_outstanding) {
        return 0;
    }
    if (f->test_active && !test_running(f, now_ms)) {
        f->test_active = false;
    }

    memset(out, 0, f->out_len);
    if (f->stops_pending) {
        for (uint8_t i = 0; i < sizeof(stop_blocks); i++) {
            if (f->stops_pending & (1u << i)) {
                out[0] = SM_EFFECT_OP;
                out[1] = stop_blocks[i];
                out[2] = SM_OP_STOP;
                f->pending_stop_bit = (uint8_t) (1u << i);
                f->outstanding_kind = KIND_STOP;
                f->packet_outstanding = true;
                return f->out_len;
            }
        }
    }

    bool want_run = f->test_active || console_wants_run(f);
    if (want_run) {
        int16_t mag = ffb_target_mag(f, now_ms);
        if (!f->uploaded) {
            static const uint8_t set_effect[13] = {
                SM_SET_EFFECT, SM_BLOCK_CONSTANT, 0x01, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0x04, 0x3F,
            };
            memcpy(out, set_effect, sizeof(set_effect));
            f->outstanding_kind = KIND_UPLOAD;
        } else if (!f->mag_valid || mag != f->sent_mag || !f->running) {
            if (!f->mag_valid || mag != f->sent_mag) {
                out[0] = SM_SET_CONSTANT;
                out[1] = SM_BLOCK_CONSTANT;
                out[2] = (uint8_t) ((uint16_t) mag & 0xFF);
                out[3] = (uint8_t) ((uint16_t) mag >> 8);
                f->pending_mag = mag;
                f->outstanding_kind = KIND_CONST;
            } else {
                out[0] = SM_EFFECT_OP;
                out[1] = SM_BLOCK_CONSTANT;
                out[2] = SM_OP_START;
                out[3] = 0x01;
                f->outstanding_kind = KIND_START;
            }
        } else {
            return 0;
        }
    } else if (f->running) {
        out[0] = SM_EFFECT_OP;
        out[1] = SM_BLOCK_CONSTANT;
        out[2] = SM_OP_STOP;
        f->pending_stop_bit = STOP_CONSTANT;
        f->outstanding_kind = KIND_STOP;
    } else {
        return 0;
    }
    f->packet_outstanding = true;
    return f->out_len;
}

void ffb_packet_done(ffb_t* f, bool ok) {
    if (!f->packet_outstanding) {
        return;
    }
    f->packet_outstanding = false;
    if (!ok) {
        f->tx_failed++;
        return;  // the next ffb_next_packet() rebuilds from the current state
    }
    f->tx_packets++;
    switch (f->outstanding_kind) {
        case KIND_STOP:
            f->stops_pending &= (uint8_t) ~f->pending_stop_bit;
            if (f->pending_stop_bit == STOP_CONSTANT && f->running) {
                f->running = false;
                emit(f, FFB_EV_ACTIVE, 0, (uint16_t) f->sent_mag);
            }
            break;
        case KIND_UPLOAD:
            f->uploaded = true;
            f->mag_valid = false;  // always set the magnitude before starting
            break;
        case KIND_CONST:
            f->sent_mag = f->pending_mag;
            f->mag_valid = true;
            break;
        case KIND_START:
            f->running = true;
            emit(f, FFB_EV_ACTIVE, 1, (uint16_t) f->sent_mag);
            break;
        default:
            break;
    }
    f->outstanding_kind = KIND_NONE;
}
