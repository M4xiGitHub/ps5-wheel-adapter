// Rig input -> G29 input report. See input_map.h.
#include "input_map.h"

#include <string.h>

uint16_t input_scale_u16(int32_t v, int32_t lmin, int32_t lmax) {
    if (lmax <= lmin || v <= lmin) {
        return 0;
    }
    if (v >= lmax) {
        return 0xFFFF;
    }
    uint64_t num = (uint64_t) ((int64_t) v - lmin) * 65535u;
    uint64_t den = (uint64_t) ((int64_t) lmax - lmin);
    return (uint16_t) ((num + den / 2) / den);
}

static bool store(const uint8_t* raw, uint16_t len, uint8_t report_id, uint16_t payload_bits,
    uint8_t* dst, uint16_t* dst_len) {
    const uint8_t* p = raw;
    uint16_t n = len;
    if (report_id != 0) {
        if (len < 1 || raw[0] != report_id) {
            return false;
        }
        p++;
        n--;
    }
    if ((uint32_t) n * 8u < payload_bits) {
        return false;  // short report: keep the previous values
    }
    if (n > INPUT_MAX_PAYLOAD) {
        n = INPUT_MAX_PAYLOAD;
    }
    memcpy(dst, p, n);
    *dst_len = n;
    return true;
}

bool input_wheel_report(input_state_t* s, const uint8_t* raw, uint16_t len) {
    if (!s->wheel_layout_ok) {
        return false;
    }
    if (!store(raw, len, s->wheel_layout.report_id, s->wheel_layout.payload_bits, s->wheel_payload, &s->wheel_len)) {
        return false;
    }
    s->wheel_valid = true;
    return true;
}

bool input_pedal_report(input_state_t* s, const uint8_t* raw, uint16_t len) {
    if (!s->pedal_layout_ok) {
        return false;
    }
    if (!store(raw, len, s->pedal_layout.report_id, s->pedal_layout.payload_bits, s->pedal_payload, &s->pedal_len)) {
        return false;
    }
    s->pedal_valid = true;
    return true;
}

void input_wheel_lost(input_state_t* s) {
    s->wheel_valid = false;
    s->wheel_layout_ok = false;
}

void input_pedal_lost(input_state_t* s) {
    s->pedal_valid = false;
    s->pedal_layout_ok = false;
}

bool input_wheel_button(const input_state_t* s, uint16_t n) {
    if (!s->wheel_valid || n == 0 || n > WHEEL_MAX_BUTTONS) {
        return false;
    }
    uint16_t ofs = s->wheel_layout.button_ofs[n - 1];
    uint32_t v;
    if (ofs == 0xFFFF || !hid_extract_bits(s->wheel_payload, s->wheel_len, ofs, 1, &v)) {
        return false;
    }
    return v != 0;
}

bool input_raw_steer(const input_state_t* s, int32_t* v) {
    return s->wheel_valid && hid_extract(s->wheel_payload, s->wheel_len, &s->wheel_layout.steer, v);
}

bool input_raw_hat(const input_state_t* s, int32_t* v) {
    return s->wheel_valid && hid_extract(s->wheel_payload, s->wheel_len, &s->wheel_layout.hat, v);
}

bool input_raw_throttle(const input_state_t* s, int32_t* v) {
    return s->pedal_valid && hid_extract(s->pedal_payload, s->pedal_len, &s->pedal_layout.z, v);
}

bool input_raw_brake(const input_state_t* s, int32_t* v) {
    return s->pedal_valid && hid_extract(s->pedal_payload, s->pedal_len, &s->pedal_layout.y, v);
}

bool input_raw_clutch(const input_state_t* s, int32_t* v) {
    return s->pedal_valid && hid_extract(s->pedal_payload, s->pedal_len, &s->pedal_layout.x, v);
}

static uint8_t hat_to_dpad(const hid_field_t* f, int32_t v) {
    int32_t d = v - f->logical_min;
    if (f->logical_max - f->logical_min >= 7 && d >= 0 && d <= 7) {
        return (uint8_t) d;  // 0 = north, clockwise
    }
    return G29_DPAD_NEUTRAL;  // null state
}

static uint8_t buttons_to_dpad(bool up, bool right, bool down, bool left) {
    if (up && down) {
        up = down = false;
    }
    if (left && right) {
        left = right = false;
    }
    if (up) {
        return right ? 1 : left ? 7 : 0;
    }
    if (down) {
        return right ? 3 : left ? 5 : 4;
    }
    if (right) {
        return 2;
    }
    if (left) {
        return 6;
    }
    return G29_DPAD_NEUTRAL;
}

static uint16_t pedal_value(const input_state_t* s, const hid_field_t* f, bool rise_when_pressed) {
    int32_t v;
    if (!hid_extract(s->pedal_payload, s->pedal_len, f, &v)) {
        return 0xFFFF;  // released
    }
    uint16_t scaled = input_scale_u16(v, f->logical_min, f->logical_max);
    return rise_when_pressed ? (uint16_t) (0xFFFF - scaled) : scaled;
}

void input_build_report(const input_state_t* s, const input_config_t* cfg, g29_report_t* out) {
    g29_report_neutral(out);

    if (s->wheel_valid) {
        int32_t v;
        if (hid_extract(s->wheel_payload, s->wheel_len, &s->wheel_layout.steer, &v)) {
            uint16_t w = input_scale_u16(v, s->wheel_layout.steer.logical_min, s->wheel_layout.steer.logical_max);
            if (cfg->steer_invert) {
                w = (uint16_t) (0xFFFF - w);
            }
            g29_set_u16(out, G29_OFS_WHEEL, w);
        }
        uint8_t dir = G29_DPAD_NEUTRAL;
        if (s->wheel_layout.hat.present && hid_extract(s->wheel_payload, s->wheel_len, &s->wheel_layout.hat, &v)) {
            dir = hat_to_dpad(&s->wheel_layout.hat, v);
        }
        bool up = false, right = false, down = false, left = false;
        for (uint8_t i = 0; i < cfg->map_len; i++) {
            const button_map_entry_t* e = &cfg->map[i];
            if (!input_wheel_button(s, e->source_button)) {
                continue;
            }
            switch (e->target) {
                case G29_BTN_DPAD_UP: up = true; break;
                case G29_BTN_DPAD_RIGHT: right = true; break;
                case G29_BTN_DPAD_DOWN: down = true; break;
                case G29_BTN_DPAD_LEFT: left = true; break;
                default: g29_set_button(out, e->target, true); break;
            }
        }
        if (dir == G29_DPAD_NEUTRAL) {
            dir = buttons_to_dpad(up, right, down, left);
        }
        g29_set_dpad(out, dir);
    }

    if (s->pedal_valid) {
        g29_set_u16(out, G29_OFS_THROTTLE, pedal_value(s, &s->pedal_layout.z, cfg->pedals_rise_when_pressed));
        g29_set_u16(out, G29_OFS_BRAKE, pedal_value(s, &s->pedal_layout.y, cfg->pedals_rise_when_pressed));
        // Clutch stays at 0xFFFF (released) from g29_report_neutral(): CFG_CLUTCH_ENABLED is 0.
    }
}
