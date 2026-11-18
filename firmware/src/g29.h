// Console-facing Logitech G29 (PS4 mode, 046D:C260) identity.
//
// The descriptor bytes, the input report layout and the fixed feature replies are
// taken from jfedor2/wheel-adapter (MIT, Copyright (c) 2026 Jacek Fedorynski),
// whose author reports PS5/GT7 operation with them. See docs/CONSOLE_BEHAVIOR.md.
#ifndef G29_H
#define G29_H

#include <stdbool.h>
#include <stdint.h>

#define G29_VID 0x046D
#define G29_PID 0xC260
#define G29_BCD_DEVICE 0x0100

#define G29_INPUT_REPORT_ID 1
#define G29_INPUT_PAYLOAD_LEN 63  // bytes after the report ID

// Byte offsets inside the 63-byte input payload (report ID excluded).
#define G29_OFS_LX 0
#define G29_OFS_LY 1
#define G29_OFS_RX 2
#define G29_OFS_RY 3
#define G29_OFS_BTN0 4  // bits 0-3 dpad, 4 square, 5 cross, 6 circle, 7 triangle
#define G29_OFS_BTN1 5  // L1 R1 L2 R2 share options L3 R3
#define G29_OFS_BTN2 6  // bit 0 PS, bit 1 touchpad, bits 2-7 counter
#define G29_OFS_WHEEL 42  // uint16 LE, 0 = full left, 0x8000 centre
#define G29_OFS_THROTTLE 44  // uint16 LE, 0xFFFF released, 0 fully pressed
#define G29_OFS_BRAKE 46
#define G29_OFS_CLUTCH 48
#define G29_OFS_GEARS 50
#define G29_OFS_BTN3 53  // bit 0 enter, 1 dial down, 2 dial up, 3 minus, 4 plus

#define G29_DPAD_NEUTRAL 8

typedef enum {
    G29_BTN_NONE = 0,
    G29_BTN_SQUARE,
    G29_BTN_CROSS,
    G29_BTN_CIRCLE,
    G29_BTN_TRIANGLE,
    G29_BTN_L1,
    G29_BTN_R1,
    G29_BTN_L2,
    G29_BTN_R2,
    G29_BTN_SHARE,
    G29_BTN_OPTIONS,
    G29_BTN_L3,
    G29_BTN_R3,
    G29_BTN_PS,
    G29_BTN_TOUCHPAD,
    G29_BTN_ENTER,
    G29_BTN_DIAL_DOWN,
    G29_BTN_DIAL_UP,
    G29_BTN_MINUS,
    G29_BTN_PLUS,
    G29_BTN_DPAD_UP,  // combined with the source hat
    G29_BTN_DPAD_RIGHT,
    G29_BTN_DPAD_DOWN,
    G29_BTN_DPAD_LEFT,
    G29_BTN_COUNT
} g29_button_t;

typedef struct {
    uint8_t b[G29_INPUT_PAYLOAD_LEN];
} g29_report_t;

void g29_report_neutral(g29_report_t* r);
void g29_set_button(g29_report_t* r, g29_button_t btn, bool pressed);
bool g29_get_button(const g29_report_t* r, g29_button_t btn);
void g29_set_dpad(g29_report_t* r, uint8_t dir);  // 0-7 clockwise from north, 8 neutral
uint8_t g29_get_dpad(const g29_report_t* r);
void g29_set_u16(g29_report_t* r, uint8_t ofs, uint16_t v);
uint16_t g29_get_u16(const g29_report_t* r, uint8_t ofs);
const char* g29_button_name(g29_button_t btn);

// HID report descriptor presented to the console.
#define G29_HID_REPORT_DESC_LEN 160
extern const uint8_t g29_hid_report_desc[G29_HID_REPORT_DESC_LEN];
extern const uint16_t g29_hid_report_desc_len;

// Fixed GET_REPORT(feature) replies, payload only (report ID excluded).
extern const uint8_t g29_feature_0x03[47];
extern const uint8_t g29_feature_0xf3[7];

#endif
