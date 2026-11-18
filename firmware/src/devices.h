// Downstream device identification and descriptor-based layout validation (pure C).
#ifndef DEVICES_H
#define DEVICES_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_desc.h"

typedef enum {
    ROLE_NONE = 0,
    ROLE_UNKNOWN,   // not in the table: enumerated, never used
    ROLE_HUB,
    ROLE_AUTH,      // candidate auth controller; must also pass auth_desc_check()
    ROLE_WHEEL,     // steering, hat, buttons
    ROLE_PEDALS,    // throttle, brake (clutch disabled)
    ROLE_PASSIVE,   // known, kept enumerated, not translated
} dev_role_t;

// TinyUSB root-hub port numbers for the two PIO-USB host ports.
#define PORT_A_RHPORT 1  // GP0 = D+, GP1 = D-   (Hori)
#define PORT_B_RHPORT 2  // GP2 = D+, GP3 = D-   (rig hub)

typedef struct {
    uint16_t vid;
    uint16_t pid;
    dev_role_t role;
    uint8_t expected_rhport;
    uint8_t expected_behind_hub;  // 1 if it should sit behind the rig hub
    const char* name;
} known_device_t;

extern const known_device_t known_devices[];
extern const uint8_t known_devices_count;

const known_device_t* devices_lookup(uint16_t vid, uint16_t pid);
const char* role_name(dev_role_t r);

// ---------------------------------------------------------------- auth controller
typedef struct {
    uint16_t bits_f0, bits_f1, bits_f2, bits_f3;  // feature payload sizes found
} auth_desc_info_t;

// True if the descriptor declares feature reports F0, F1 (>= 63 bytes), F2 (>= 15)
// and F3 (>= 7), i.e. the interface a licensed PS4 controller uses for signing.
bool auth_desc_check(const uint8_t* desc, uint16_t len, auth_desc_info_t* info);

// ---------------------------------------------------------------- wheel
#define WHEEL_MAX_BUTTONS 128

typedef struct {
    uint8_t report_id;
    uint16_t payload_bits;
    hid_field_t steer;  // Generic Desktop X
    hid_field_t hat;    // Generic Desktop Hat switch
    uint16_t button_ofs[WHEEL_MAX_BUTTONS];  // button n at button_ofs[n-1], 0xFFFF absent
    uint16_t button_count;  // highest button usage found
} wheel_layout_t;

// Mismatch bits vs the documented Simagic 0483:0522 layout.
#define LAYOUT_MM_REPORT 0x01  // report ID / payload size
#define LAYOUT_MM_AXIS1 0x02   // wheel: steering; pedals: X
#define LAYOUT_MM_AXIS2 0x04   // wheel: hat;      pedals: Y
#define LAYOUT_MM_AXIS3 0x08   // wheel: buttons;  pedals: Z

typedef enum {
    LAYOUT_MATCHES_DOC = 0,  // descriptor confirms the documented layout
    LAYOUT_DIFFERS = 1,      // usable, but differs from the documented layout (descriptor wins)
    LAYOUT_UNUSABLE = 2,     // required fields missing
} layout_status_t;

layout_status_t wheel_layout_from_desc(const uint8_t* desc, uint16_t len, wheel_layout_t* out, uint8_t* mismatch);

// Documented: report ID 1, 39-byte packet incl. ID; steering u16 at bytes 1-2;
// hat = low nibble of byte 12; 116 buttons from packet bit 100.
#define SIMAGIC_DOC_REPORT_ID 1
#define SIMAGIC_DOC_PAYLOAD_BITS (38 * 8)
#define SIMAGIC_DOC_STEER_OFS 0
#define SIMAGIC_DOC_HAT_OFS 88
#define SIMAGIC_DOC_BTN1_OFS 92
#define SIMAGIC_DOC_BUTTONS 116

// ---------------------------------------------------------------- pedals
typedef struct {
    uint8_t report_id;
    uint16_t payload_bits;
    hid_field_t x, y, z;
} pedal_layout_t;

layout_status_t pedal_layout_from_desc(const uint8_t* desc, uint16_t len, pedal_layout_t* out, uint8_t* mismatch);

// Documented: report ID 1, 64-byte packet incl. ID, X/Y/Z u16 LE at bytes 1-2, 3-4, 5-6.
#define VRS_DOC_REPORT_ID 1
#define VRS_DOC_PAYLOAD_BITS (63 * 8)

// ---------------------------------------------------------------- routing decision
// Decides what one mounted HID interface is used for. A role is granted only when
// the VID/PID candidate role is confirmed by the interface's own report descriptor:
//   ROLE_AUTH   known auth VID/PID and auth_desc_check() passes
//   ROLE_WHEEL  known wheel VID/PID and a usable wheel layout
//   ROLE_PEDALS known pedal VID/PID and a usable pedal layout
// Anything else (other interfaces of the same devices, hub, Stream Deck, unknown
// devices) gets ROLE_PASSIVE / ROLE_UNKNOWN / ROLE_HUB and is never sent requests.
typedef struct {
    dev_role_t candidate;     // from VID/PID
    dev_role_t role;          // effective role of this interface
    layout_status_t layout;   // wheel/pedals only
    uint8_t mismatch;         // wheel/pedals only
    auth_desc_info_t auth;    // auth only
} hid_decision_t;

hid_decision_t devices_decide_hid(uint16_t vid, uint16_t pid, const uint8_t* desc, uint16_t len,
    wheel_layout_t* wl, pedal_layout_t* pl);

// True if the device sits where the topology says it should.
bool devices_port_ok(const known_device_t* k, uint8_t rhport, uint8_t hub_addr);

#endif
