// Downstream device identification and layout validation. See devices.h.
#include "devices.h"

#include <stddef.h>
#include <string.h>

const known_device_t known_devices[] = {
    { 0x0F0D, 0x0087, ROLE_AUTH, PORT_A_RHPORT, 0, "Hori 0F0D:0087" },
    { 0x05E3, 0x0610, ROLE_HUB, PORT_B_RHPORT, 0, "Rig hub 05E3:0610" },
    { 0x0483, 0x0522, ROLE_WHEEL, PORT_B_RHPORT, 1, "Simagic 0483:0522" },
    { 0x0483, 0xA3BE, ROLE_PEDALS, PORT_B_RHPORT, 1, "VRS pedals 0483:A3BE" },
    { 0x0FD9, 0x0063, ROLE_PASSIVE, PORT_B_RHPORT, 1, "Stream Deck Mini 0FD9:0063" },
    { 0x8087, 0x1024, ROLE_PASSIVE, PORT_B_RHPORT, 1, "HID 8087:1024 (unverified)" },
};
const uint8_t known_devices_count = sizeof(known_devices) / sizeof(known_devices[0]);

const known_device_t* devices_lookup(uint16_t vid, uint16_t pid) {
    for (uint8_t i = 0; i < known_devices_count; i++) {
        if (known_devices[i].vid == vid && known_devices[i].pid == pid) {
            return &known_devices[i];
        }
    }
    return NULL;
}

const char* role_name(dev_role_t r) {
    switch (r) {
        case ROLE_NONE: return "none";
        case ROLE_UNKNOWN: return "unknown";
        case ROLE_HUB: return "hub";
        case ROLE_AUTH: return "auth";
        case ROLE_WHEEL: return "wheel";
        case ROLE_PEDALS: return "pedals";
        case ROLE_PASSIVE: return "passive";
    }
    return "?";
}

bool auth_desc_check(const uint8_t* desc, uint16_t len, auth_desc_info_t* info) {
    auth_desc_info_t i;
    i.bits_f0 = (uint16_t) hid_desc_report_bits(desc, len, HID_RPT_FEATURE, 0xF0);
    i.bits_f1 = (uint16_t) hid_desc_report_bits(desc, len, HID_RPT_FEATURE, 0xF1);
    i.bits_f2 = (uint16_t) hid_desc_report_bits(desc, len, HID_RPT_FEATURE, 0xF2);
    i.bits_f3 = (uint16_t) hid_desc_report_bits(desc, len, HID_RPT_FEATURE, 0xF3);
    if (info != NULL) {
        *info = i;
    }
    return i.bits_f0 >= 63 * 8 && i.bits_f1 >= 63 * 8 && i.bits_f2 >= 15 * 8 && i.bits_f3 >= 7 * 8;
}

layout_status_t wheel_layout_from_desc(const uint8_t* desc, uint16_t len, wheel_layout_t* out, uint8_t* mismatch) {
    memset(out, 0, sizeof(*out));
    *mismatch = 0;
    if (!hid_desc_find_usage(desc, len, HID_RPT_INPUT, HID_PAGE_GENERIC_DESKTOP, HID_USAGE_GD_X, &out->steer) ||
        out->steer.bit_size < 8 || out->steer.bit_size > 32 || out->steer.logical_max <= out->steer.logical_min) {
        for (uint16_t b = 0; b < WHEEL_MAX_BUTTONS; b++) {
            out->button_ofs[b] = 0xFFFF;
        }
        return LAYOUT_UNUSABLE;
    }
    out->report_id = out->steer.report_id;
    out->payload_bits = (uint16_t) hid_desc_report_bits(desc, len, HID_RPT_INPUT, out->report_id);

    hid_field_t hat;
    if (hid_desc_find_usage(desc, len, HID_RPT_INPUT, HID_PAGE_GENERIC_DESKTOP, HID_USAGE_GD_HAT, &hat) &&
        hat.report_id == out->report_id) {
        out->hat = hat;
    }
    out->button_count = hid_desc_button_offsets(desc, len, out->report_id, out->button_ofs, WHEEL_MAX_BUTTONS);

    if (out->report_id != SIMAGIC_DOC_REPORT_ID || out->payload_bits != SIMAGIC_DOC_PAYLOAD_BITS) {
        *mismatch |= LAYOUT_MM_REPORT;
    }
    if (out->steer.bit_offset != SIMAGIC_DOC_STEER_OFS || out->steer.bit_size != 16 || out->steer.logical_min < 0) {
        *mismatch |= LAYOUT_MM_AXIS1;
    }
    if (!out->hat.present || out->hat.bit_offset != SIMAGIC_DOC_HAT_OFS || out->hat.bit_size != 4) {
        *mismatch |= LAYOUT_MM_AXIS2;
    }
    bool buttons_ok = out->button_count == SIMAGIC_DOC_BUTTONS;
    for (uint16_t b = 0; buttons_ok && b < SIMAGIC_DOC_BUTTONS; b++) {
        buttons_ok = out->button_ofs[b] == SIMAGIC_DOC_BTN1_OFS + b;
    }
    if (!buttons_ok) {
        *mismatch |= LAYOUT_MM_AXIS3;
    }
    return *mismatch ? LAYOUT_DIFFERS : LAYOUT_MATCHES_DOC;
}

layout_status_t pedal_layout_from_desc(const uint8_t* desc, uint16_t len, pedal_layout_t* out, uint8_t* mismatch) {
    memset(out, 0, sizeof(*out));
    *mismatch = 0;
    hid_desc_find_usage(desc, len, HID_RPT_INPUT, HID_PAGE_GENERIC_DESKTOP, HID_USAGE_GD_X, &out->x);
    hid_desc_find_usage(desc, len, HID_RPT_INPUT, HID_PAGE_GENERIC_DESKTOP, HID_USAGE_GD_Y, &out->y);
    hid_desc_find_usage(desc, len, HID_RPT_INPUT, HID_PAGE_GENERIC_DESKTOP, HID_USAGE_GD_Z, &out->z);
    // Throttle (Z) and brake (Y) are required and must share one report.
    if (!out->y.present || !out->z.present || out->y.report_id != out->z.report_id ||
        out->y.logical_max <= out->y.logical_min || out->z.logical_max <= out->z.logical_min ||
        out->y.bit_size > 32 || out->z.bit_size > 32) {
        return LAYOUT_UNUSABLE;
    }
    out->report_id = out->z.report_id;
    out->payload_bits = (uint16_t) hid_desc_report_bits(desc, len, HID_RPT_INPUT, out->report_id);
    if (out->x.present && out->x.report_id != out->report_id) {
        out->x.present = false;
    }
    if (out->report_id != VRS_DOC_REPORT_ID || out->payload_bits != VRS_DOC_PAYLOAD_BITS) {
        *mismatch |= LAYOUT_MM_REPORT;
    }
    if (!out->x.present || out->x.bit_offset != 0 || out->x.bit_size != 16) {
        *mismatch |= LAYOUT_MM_AXIS1;
    }
    if (out->y.bit_offset != 16 || out->y.bit_size != 16) {
        *mismatch |= LAYOUT_MM_AXIS2;
    }
    if (out->z.bit_offset != 32 || out->z.bit_size != 16) {
        *mismatch |= LAYOUT_MM_AXIS3;
    }
    return *mismatch ? LAYOUT_DIFFERS : LAYOUT_MATCHES_DOC;
}

hid_decision_t devices_decide_hid(uint16_t vid, uint16_t pid, const uint8_t* desc, uint16_t len,
    wheel_layout_t* wl, pedal_layout_t* pl) {
    hid_decision_t d;
    memset(&d, 0, sizeof(d));
    const known_device_t* k = devices_lookup(vid, pid);
    d.candidate = k ? k->role : ROLE_UNKNOWN;
    d.role = (d.candidate == ROLE_AUTH || d.candidate == ROLE_WHEEL || d.candidate == ROLE_PEDALS) ? ROLE_PASSIVE
                                                                                                    : d.candidate;
    d.layout = LAYOUT_UNUSABLE;
    switch (d.candidate) {
        case ROLE_AUTH:
            if (auth_desc_check(desc, len, &d.auth)) {
                d.role = ROLE_AUTH;
            }
            break;
        case ROLE_WHEEL:
            d.layout = wheel_layout_from_desc(desc, len, wl, &d.mismatch);
            if (d.layout != LAYOUT_UNUSABLE) {
                d.role = ROLE_WHEEL;
            }
            break;
        case ROLE_PEDALS:
            d.layout = pedal_layout_from_desc(desc, len, pl, &d.mismatch);
            if (d.layout != LAYOUT_UNUSABLE) {
                d.role = ROLE_PEDALS;
            }
            break;
        default:
            break;
    }
    return d;
}

bool devices_port_ok(const known_device_t* k, uint8_t rhport, uint8_t hub_addr) {
    return k != NULL && rhport == k->expected_rhport && ((hub_addr != 0) == (k->expected_behind_hub != 0));
}
