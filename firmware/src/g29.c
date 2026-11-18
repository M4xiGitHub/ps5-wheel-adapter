// Console-facing Logitech G29 identity. See g29.h.
//
// g29_hid_report_desc, g29_feature_0x03 and g29_feature_0xf3 are copied byte for
// byte from jfedor2/wheel-adapter r2026-11-07 src/descriptors.c and src/adapter.c:
//   Copyright (c) 2026 Jacek Fedorynski
//   SPDX-License-Identifier: MIT
#include "g29.h"

#include <string.h>

void g29_report_neutral(g29_report_t* r) {
    memset(r->b, 0, sizeof(r->b));
    r->b[G29_OFS_LX] = 0x80;
    r->b[G29_OFS_LY] = 0x80;
    r->b[G29_OFS_RX] = 0x80;
    r->b[G29_OFS_RY] = 0x80;
    g29_set_dpad(r, G29_DPAD_NEUTRAL);
    g29_set_u16(r, G29_OFS_WHEEL, 0x8000);
    g29_set_u16(r, G29_OFS_THROTTLE, 0xFFFF);
    g29_set_u16(r, G29_OFS_BRAKE, 0xFFFF);
    g29_set_u16(r, G29_OFS_CLUTCH, 0xFFFF);
}

void g29_set_u16(g29_report_t* r, uint8_t ofs, uint16_t v) {
    r->b[ofs] = (uint8_t) (v & 0xFF);
    r->b[ofs + 1] = (uint8_t) (v >> 8);
}

uint16_t g29_get_u16(const g29_report_t* r, uint8_t ofs) {
    return (uint16_t) (r->b[ofs] | (r->b[ofs + 1] << 8));
}

void g29_set_dpad(g29_report_t* r, uint8_t dir) {
    if (dir > 8) {
        dir = 8;
    }
    r->b[G29_OFS_BTN0] = (uint8_t) ((r->b[G29_OFS_BTN0] & 0xF0) | dir);
}

uint8_t g29_get_dpad(const g29_report_t* r) {
    return r->b[G29_OFS_BTN0] & 0x0F;
}

static bool button_location(g29_button_t btn, uint8_t* ofs, uint8_t* bit) {
    switch (btn) {
        case G29_BTN_SQUARE: *ofs = G29_OFS_BTN0; *bit = 4; return true;
        case G29_BTN_CROSS: *ofs = G29_OFS_BTN0; *bit = 5; return true;
        case G29_BTN_CIRCLE: *ofs = G29_OFS_BTN0; *bit = 6; return true;
        case G29_BTN_TRIANGLE: *ofs = G29_OFS_BTN0; *bit = 7; return true;
        case G29_BTN_L1: *ofs = G29_OFS_BTN1; *bit = 0; return true;
        case G29_BTN_R1: *ofs = G29_OFS_BTN1; *bit = 1; return true;
        case G29_BTN_L2: *ofs = G29_OFS_BTN1; *bit = 2; return true;
        case G29_BTN_R2: *ofs = G29_OFS_BTN1; *bit = 3; return true;
        case G29_BTN_SHARE: *ofs = G29_OFS_BTN1; *bit = 4; return true;
        case G29_BTN_OPTIONS: *ofs = G29_OFS_BTN1; *bit = 5; return true;
        case G29_BTN_L3: *ofs = G29_OFS_BTN1; *bit = 6; return true;
        case G29_BTN_R3: *ofs = G29_OFS_BTN1; *bit = 7; return true;
        case G29_BTN_PS: *ofs = G29_OFS_BTN2; *bit = 0; return true;
        case G29_BTN_TOUCHPAD: *ofs = G29_OFS_BTN2; *bit = 1; return true;
        case G29_BTN_ENTER: *ofs = G29_OFS_BTN3; *bit = 0; return true;
        case G29_BTN_DIAL_DOWN: *ofs = G29_OFS_BTN3; *bit = 1; return true;
        case G29_BTN_DIAL_UP: *ofs = G29_OFS_BTN3; *bit = 2; return true;
        case G29_BTN_MINUS: *ofs = G29_OFS_BTN3; *bit = 3; return true;
        case G29_BTN_PLUS: *ofs = G29_OFS_BTN3; *bit = 4; return true;
        default: return false;  // dpad pseudo-buttons are folded into the hat
    }
}

void g29_set_button(g29_report_t* r, g29_button_t btn, bool pressed) {
    uint8_t ofs, bit;
    if (!button_location(btn, &ofs, &bit)) {
        return;
    }
    if (pressed) {
        r->b[ofs] |= (uint8_t) (1u << bit);
    } else {
        r->b[ofs] &= (uint8_t) ~(1u << bit);
    }
}

bool g29_get_button(const g29_report_t* r, g29_button_t btn) {
    uint8_t ofs, bit;
    if (!button_location(btn, &ofs, &bit)) {
        return false;
    }
    return (r->b[ofs] >> bit) & 1u;
}

const char* g29_button_name(g29_button_t btn) {
    static const char* const names[G29_BTN_COUNT] = {
        "none", "square", "cross", "circle", "triangle", "L1", "R1", "L2", "R2",
        "share", "options", "L3", "R3", "PS", "touchpad", "enter", "dial_down",
        "dial_up", "minus", "plus", "dpad_up", "dpad_right", "dpad_down", "dpad_left",
    };
    return (btn < G29_BTN_COUNT) ? names[btn] : "?";
}

const uint8_t g29_hid_report_desc[G29_HID_REPORT_DESC_LEN] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (English Rotation, Centimeter)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null State)
    0x65, 0x00,        //   Unit (None)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0E,        //   Usage Maximum (0x0E)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0E,        //   Report Count (14)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20,        //   Usage (0x20)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21,        //   Usage (0x21)
    0x95, 0x36,        //   Report Count (54)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x05,        //   Report ID (5)
    0x09, 0x22,        //   Usage (0x22)
    0x95, 0x1F,        //   Report Count (31)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x85, 0x03,        //   Report ID (3)
    0x0A, 0x21, 0x27,  //   Usage (0x2721)
    0x95, 0x2F,        //   Report Count (47)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0xC0,              // End Collection
    0x06, 0xF0, 0xFF,  // Usage Page (Vendor Defined 0xFFF0)
    0x09, 0x40,        // Usage (0x40)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0xF0,        //   Report ID (0xF0) auth: nonce from console
    0x09, 0x47,        //   Usage (0x47)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF1,        //   Report ID (0xF1) auth: signature to console
    0x09, 0x48,        //   Usage (0x48)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF2,        //   Report ID (0xF2) auth: signing status
    0x09, 0x49,        //   Usage (0x49)
    0x95, 0x0F,        //   Report Count (15)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF3,        //   Report ID (0xF3) auth: reset / page sizes
    0x0A, 0x01, 0x47,  //   Usage (0x4701)
    0x95, 0x07,        //   Report Count (7)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0xC0,              // End Collection
};
const uint16_t g29_hid_report_desc_len = sizeof(g29_hid_report_desc);
_Static_assert(sizeof(g29_hid_report_desc) == G29_HID_REPORT_DESC_LEN, "descriptor length");

const uint8_t g29_feature_0x03[47] = {
    0x21, 0x27, 0x03, 0x11, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0D, 0x0D, 0x00, 0x00, 0x00, 0x00,
    0x0D, 0x84, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t g29_feature_0xf3[7] = { 0x00, 0x38, 0x38, 0x00, 0x00, 0x00, 0x00 };
