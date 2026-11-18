// Rig input (Simagic wheel + VRS pedals) -> G29 input report (pure C).
#ifndef INPUT_MAP_H
#define INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "devices.h"
#include "g29.h"

typedef struct {
    uint8_t source_button;  // 1-based
    g29_button_t target;
} button_map_entry_t;

typedef struct {
    const button_map_entry_t* map;
    uint8_t map_len;
    bool steer_invert;
    bool pedals_rise_when_pressed;
} input_config_t;

#define INPUT_MAX_PAYLOAD 64

typedef struct {
    // layouts (from the devices' report descriptors)
    bool wheel_layout_ok;
    wheel_layout_t wheel_layout;
    bool pedal_layout_ok;
    pedal_layout_t pedal_layout;

    // latest payloads (report ID byte excluded); valid only after the first report
    bool wheel_valid;
    uint16_t wheel_len;
    uint8_t wheel_payload[INPUT_MAX_PAYLOAD];
    bool pedal_valid;
    uint16_t pedal_len;
    uint8_t pedal_payload[INPUT_MAX_PAYLOAD];
} input_state_t;

// Linear, full-resolution mapping of [lmin, lmax] onto [0, 65535] with clamping.
uint16_t input_scale_u16(int32_t v, int32_t lmin, int32_t lmax);

// Stores a raw report (report ID at raw[0] if the layout uses report IDs).
// Returns false and leaves the state unchanged if the report is not the input
// report described by the layout.
bool input_wheel_report(input_state_t* s, const uint8_t* raw, uint16_t len);
bool input_pedal_report(input_state_t* s, const uint8_t* raw, uint16_t len);
void input_wheel_lost(input_state_t* s);
void input_pedal_lost(input_state_t* s);

// Source button state (1-based); false when unknown.
bool input_wheel_button(const input_state_t* s, uint16_t n);

// Builds the full G29 report. Missing sources give neutral values.
void input_build_report(const input_state_t* s, const input_config_t* cfg, g29_report_t* out);

// Raw source values for diagnostics; return false when unavailable.
bool input_raw_steer(const input_state_t* s, int32_t* v);
bool input_raw_hat(const input_state_t* s, int32_t* v);
bool input_raw_throttle(const input_state_t* s, int32_t* v);
bool input_raw_brake(const input_state_t* s, int32_t* v);
bool input_raw_clutch(const input_state_t* s, int32_t* v);

#endif
