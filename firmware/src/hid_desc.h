// HID report descriptor parser (pure C, no SDK dependencies).
//
// Used to validate the input layouts of the rig devices and the authentication
// interface of the Hori against their own report descriptors, instead of
// trusting VID/PID or hard-coded offsets.
#ifndef HID_DESC_H
#define HID_DESC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HID_RPT_INPUT = 1,
    HID_RPT_OUTPUT = 2,
    HID_RPT_FEATURE = 3,
} hid_rpt_type_t;

#define HID_PAGE_GENERIC_DESKTOP 0x01
#define HID_PAGE_SIMULATION 0x02
#define HID_PAGE_BUTTON 0x09

#define HID_USAGE_GD_X 0x30
#define HID_USAGE_GD_Y 0x31
#define HID_USAGE_GD_Z 0x32
#define HID_USAGE_GD_RX 0x33
#define HID_USAGE_GD_RY 0x34
#define HID_USAGE_GD_RZ 0x35
#define HID_USAGE_GD_HAT 0x39

// Main-item data flags.
#define HID_FLAG_CONSTANT 0x01
#define HID_FLAG_VARIABLE 0x02
#define HID_FLAG_RELATIVE 0x04
#define HID_FLAG_NULL_STATE 0x40

// One element (one report-count slot) of a main item.
typedef struct {
    hid_rpt_type_t type;
    uint8_t report_id;     // 0 when the descriptor uses no report IDs
    uint16_t usage_page;
    uint16_t usage;
    uint16_t bit_offset;   // within the report payload, i.e. after the report ID byte
    uint8_t bit_size;
    uint16_t flags;
    int32_t logical_min;
    int32_t logical_max;
} hid_element_t;

// Location of one value in a report.
typedef struct {
    bool present;
    uint8_t report_id;
    uint16_t bit_offset;  // within the payload (report ID byte excluded)
    uint8_t bit_size;
    int32_t logical_min;
    int32_t logical_max;
    uint16_t flags;
} hid_field_t;

typedef void (*hid_element_cb_t)(void* ctx, const hid_element_t* el);

// Walks the descriptor and calls cb for every element of every Input/Output/Feature
// item. Returns false on a malformed descriptor (truncated item, bad push/pop,
// oversized report). Elements reported before the error remain valid.
bool hid_desc_walk(const uint8_t* desc, uint16_t len, hid_element_cb_t cb, void* ctx);

// Finds the first variable element with the given usage in reports of the given type.
bool hid_desc_find_usage(const uint8_t* desc, uint16_t len, hid_rpt_type_t type,
    uint16_t usage_page, uint16_t usage, hid_field_t* out);

// Payload size in bits (report ID byte excluded) of one report; 0 if absent.
uint32_t hid_desc_report_bits(const uint8_t* desc, uint16_t len, hid_rpt_type_t type, uint8_t report_id);

// Bit offsets of Button usages 1..max_buttons (offsets[i] is button i+1) in the
// given input report. Absent buttons get 0xFFFF. Returns the highest button found.
uint16_t hid_desc_button_offsets(const uint8_t* desc, uint16_t len, uint8_t report_id,
    uint16_t* offsets, uint16_t max_buttons);

// Extracts a field value from a report payload (report ID byte excluded).
// Values are sign-extended when the field's logical minimum is negative.
// Returns false when the field lies outside the payload.
bool hid_extract(const uint8_t* payload, uint16_t payload_len, const hid_field_t* f, int32_t* value);

// Extracts an unsigned bit field (used for single buttons).
bool hid_extract_bits(const uint8_t* payload, uint16_t payload_len, uint16_t bit_offset, uint8_t bit_size, uint32_t* value);

#endif
