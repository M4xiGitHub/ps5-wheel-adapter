// PS5 sim-racing adapter: Raspberry Pi Pico (RP2040).
//
//   Port A  GP0/GP1  Pico-PIO-USB root port 0 (TinyUSB rhport 1): Hori 0F0D:0087 (auth)
//   Port B  GP2/GP3  Pico-PIO-USB root port 1 (TinyUSB rhport 2): rig hub 05E3:0610
//   Native Micro-USB (TinyUSB rhport 0): G29 046D:C260 towards the PS5
//
// Single cooperative loop on core 0, as in jfedor2/wheel-adapter: tuh_task(),
// tud_task(), then the application tasks. Every TinyUSB host operation used here
// is asynchronous; nothing in the loop waits for the bus.
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"
#include "device/dcd.h"
#include "host/hcd.h"

#include "adapter_config.h"
#include "auth.h"
#include "crc32.h"
#include "devices.h"
#include "diag.h"
#include "ffb.h"
#include "g29.h"
#include "input_map.h"

#ifndef ADAPTER_BENCH
#define ADAPTER_BENCH 0
#endif

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 2
#define FW_VERSION_PATCH 2
#define FW_VERSION ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)

#define PORT_B_DP_PIN 2
#define LED_PIN 25
#define HEARTBEAT_MS 5000
#define WATCHDOG_MS 2000

_Static_assert(CFG_FFB_STRENGTH_PERCENT >= 0 && CFG_FFB_STRENGTH_PERCENT <= 100, "FFB cap must be 0..100 %");
_Static_assert(CFG_CLUTCH_ENABLED == 0, "clutch mapping is not verified yet");

// ------------------------------------------------------------------ state

static diag_t g_diag;
static auth_t g_auth;
static ffb_t g_ffb;
static input_holds_t g_holds;
static uint32_t g_hold_next_ms;
static bool g_hold_pending;
static bool g_ffb_use_control __attribute__((unused));  // wheelbase interface has no interrupt OUT endpoint
static uint8_t g_ffb_ctrl_buf[1 + FFB_OUT_MAX] __attribute__((unused));
static input_state_t g_input;
static const button_map_entry_t g_button_map[] = { CFG_BUTTON_MAP };
static const input_config_t g_input_cfg = {
    .map = g_button_map,
    .map_len = sizeof(g_button_map) / sizeof(g_button_map[0]),
    .steer_invert = CFG_STEER_INVERT,
    .pedals_rise_when_pressed = CFG_PEDALS_RISE_WHEN_PRESSED,
};

typedef struct {
    bool used;
    uint16_t vid, pid;
    tuh_bus_info_t bus;
    dev_role_t role;
    bool mounted;  // tuh_mount_cb seen
} dev_entry_t;

static dev_entry_t g_devs[CFG_TUH_DEVICE_MAX + 1];  // indexed by device address

typedef struct {
    bool present;
    uint8_t daddr;
    uint8_t idx;
} hid_slot_t;

static hid_slot_t g_auth_dev, g_wheel, g_pedals;

// Written from interrupt context (TinyUSB event hooks), read in the loop.
static volatile uint32_t g_port_attach[3];
static volatile uint32_t g_port_remove[3];
static volatile uint32_t g_bus_resets;
static uint32_t g_port_attach_seen[3];
static uint32_t g_port_remove_seen[3];

static struct {
    uint32_t reports_sent;
    uint32_t send_busy;
    uint32_t wheel_rx;
    uint32_t pedal_rx;
    uint32_t input_rejects;
    uint32_t console_out;
} g_cnt;

static bool g_console_configured;
static bool g_suspended;
static bool g_input_dirty = true;
static g29_report_t g_report;
static g29_report_t g_last_sent;
static bool g_ps_pressed;
static uint8_t g_console_req_seen[64];  // bit per (report id, set/get)
static uint8_t g_src_buttons[WHEEL_MAX_BUTTONS / 8];
static bool g_auth_delivered_since_f3;
static bool g_auth_in_progress;

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// ------------------------------------------------------------------ logging helpers

static void log_rec(uint8_t type, uint8_t a, const uint8_t* d8, bool urgent) {
    diag_push(&g_diag, now_ms(), type, a, d8, urgent);
}

static void log8(uint8_t type, uint8_t a, uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5,
    uint8_t d6, uint8_t d7, bool urgent) {
    uint8_t d[8] = { d0, d1, d2, d3, d4, d5, d6, d7 };
    log_rec(type, a, d, urgent);
}

static void log_u16x4(uint8_t type, uint8_t a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool urgent) {
    log8(type, a, (uint8_t) w0, (uint8_t) (w0 >> 8), (uint8_t) w1, (uint8_t) (w1 >> 8), (uint8_t) w2,
        (uint8_t) (w2 >> 8), (uint8_t) w3, (uint8_t) (w3 >> 8), urgent);
}

static void auth_event(void* ctx, auth_event_t ev, uint8_t a, uint32_t b) {
    (void) ctx;
    bool routine = ev == AUTH_EV_CONSOLE_NONCE_PAGE || ev == AUTH_EV_DS_STEP_OK || ev == AUTH_EV_CONSOLE_F1_PAGE;
    log8(DREC_AUTH, (uint8_t) ev, a, (uint8_t) b, (uint8_t) (b >> 8), (uint8_t) (b >> 16), (uint8_t) (b >> 24),
        (uint8_t) g_auth.ds, 0, 0, !routine);
    switch (ev) {
        case AUTH_EV_CONSOLE_F3:
            g_auth_in_progress = true;
            g_auth_delivered_since_f3 = false;
            break;
        case AUTH_EV_DELIVERED:
            g_auth_in_progress = false;
            g_auth_delivered_since_f3 = true;
            break;
        default:
            break;
    }
}

static void ffb_event(void* ctx, ffb_event_t ev, uint8_t a, uint32_t b) {
    (void) ctx;
    log8(DREC_FFB, (uint8_t) ev, a, (uint8_t) b, (uint8_t) (b >> 8), (uint8_t) (b >> 16), (uint8_t) (b >> 24), 0, 0, 0,
        ev != FFB_EV_FIRST_CMD);
}

static void log_field(uint8_t field_id, const hid_field_t* f) {
    int32_t lmin = f->logical_min < -32768 ? -32768 : f->logical_min > 32767 ? 32767 : f->logical_min;
    uint32_t lmax = f->logical_max < 0 ? 0 : f->logical_max > 65535 ? 65535 : (uint32_t) f->logical_max;
    log8(DREC_LAYOUT_FIELD, field_id, (uint8_t) f->bit_offset, (uint8_t) (f->bit_offset >> 8), f->bit_size,
        f->report_id, (uint8_t) lmin, (uint8_t) ((uint32_t) lmin >> 8), (uint8_t) lmax, (uint8_t) (lmax >> 8),
        false);
}

// ------------------------------------------------------------------ flash backend
// The binary is linked copy_to_ram (see CMakeLists.txt): no code or constant data is
// fetched from flash while it runs, so flash programming does not stall interrupts
// (PIO-USB SOF timer, native USB) and the other core is parked in the boot ROM.

static bool fl_read(void* ctx, uint32_t ofs, void* buf, uint32_t len) {
    (void) ctx;
    memcpy(buf, (const void*) (XIP_BASE + ofs), len);
    return true;
}

static bool fl_erase(void* ctx, uint32_t ofs, uint32_t len) {
    (void) ctx;
    flash_range_erase(ofs, len);
    return true;
}

static bool fl_program(void* ctx, uint32_t ofs, const void* buf, uint32_t len) {
    (void) ctx;
    flash_range_program(ofs, (const uint8_t*) buf, len);
    return true;
}

static const diag_flash_ops_t g_flash_ops = { fl_read, fl_erase, fl_program, NULL };

extern char __flash_binary_end;

static uint8_t reset_reason(void) {
    uint32_t cr = vreg_and_chip_reset_hw->chip_reset;
    uint8_t r = 0;
    if (watchdog_caused_reboot()) r |= 0x01;
    if (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) r |= 0x02;
    if (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) r |= 0x04;
    if (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS) r |= 0x08;
    return r;
}

// ------------------------------------------------------------------ device registry

static dev_entry_t* dev_get(uint8_t daddr) {
    if (daddr == 0 || daddr > CFG_TUH_DEVICE_MAX) {
        return NULL;
    }
    dev_entry_t* e = &g_devs[daddr];
    if (!e->used) {
        memset(e, 0, sizeof(*e));
        e->used = true;
        tuh_vid_pid_get(daddr, &e->vid, &e->pid);
        tuh_bus_info_get(daddr, &e->bus);
        const known_device_t* k = devices_lookup(e->vid, e->pid);
        e->role = k ? k->role : ROLE_UNKNOWN;
        log8(DREC_DEV_MOUNT, daddr, e->bus.rhport, e->bus.hub_addr, e->bus.hub_port, e->bus.speed,
            (uint8_t) e->vid, (uint8_t) (e->vid >> 8), (uint8_t) e->pid, (uint8_t) (e->pid >> 8), true);
        bool port_ok = devices_port_ok(k, e->bus.rhport, e->bus.hub_addr);
        log8(DREC_DEV_ROLE, daddr, (uint8_t) e->role, port_ok, k ? k->expected_rhport : 0,
            k ? k->expected_behind_hub : 0, 0, 0, 0, 0, true);
    }
    return e;
}

// ------------------------------------------------------------------ TinyUSB host callbacks

void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void) in_isr;
    if (rhport > 2) {
        return;
    }
    if (eventid == HCD_EVENT_DEVICE_ATTACH) {
        g_port_attach[rhport]++;
    } else if (eventid == HCD_EVENT_DEVICE_REMOVE) {
        g_port_remove[rhport]++;
    }
}

void tuh_mount_cb(uint8_t daddr) {
    dev_entry_t* e = dev_get(daddr);
    if (e != NULL) {
        e->mounted = true;
    }
}

void tuh_umount_cb(uint8_t daddr) {
    if (daddr == 0 || daddr > CFG_TUH_DEVICE_MAX) {
        return;
    }
    log8(DREC_DEV_UMOUNT, daddr, (uint8_t) g_devs[daddr].role, 0, 0, 0, 0, 0, 0, 0, true);
    memset(&g_devs[daddr], 0, sizeof(g_devs[daddr]));
}

static void log_descriptor(uint8_t daddr, uint8_t idx, const uint8_t* desc, uint16_t len) {
    uint8_t chunks = (uint8_t) ((len + 5) / 6 > 255 ? 255 : (len + 5) / 6);
    for (uint8_t c = 0; c < chunks; c++) {
        uint8_t d[8] = { idx, c, 0, 0, 0, 0, 0, 0 };
        for (uint8_t k = 0; k < 6; k++) {
            uint32_t i = (uint32_t) c * 6u + k;
            d[2 + k] = (i < len) ? desc[i] : 0;
        }
        log_rec(DREC_HID_DESC_CHUNK, daddr, d, false);
    }
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const* desc, uint16_t len) {
    dev_entry_t* e = dev_get(daddr);
    tuh_itf_info_t info;
    memset(&info, 0, sizeof(info));
    tuh_hid_itf_get_info(daddr, idx, &info);
    uint32_t crc = crc32_calc(desc, len);
    log8(DREC_HID_MOUNT, daddr, idx, info.desc.bInterfaceNumber, (uint8_t) len, (uint8_t) (len >> 8), (uint8_t) crc,
        (uint8_t) (crc >> 8), (uint8_t) (crc >> 16), (uint8_t) (crc >> 24), true);
    log_descriptor(daddr, idx, desc, len);
    if (e == NULL) {
        return;
    }

    // Scratch layouts: only adopted if this interface gets the role (a composite
    // device may expose several HID interfaces).
    static wheel_layout_t wl;
    static pedal_layout_t pl;
    hid_decision_t dec = devices_decide_hid(e->vid, e->pid, desc, len, &wl, &pl);

    switch (dec.candidate) {
        case ROLE_AUTH:
            log_u16x4(DREC_AUTH_DESC, daddr, (uint32_t) (dec.role == ROLE_AUTH) | ((uint32_t) idx << 8),
                dec.auth.bits_f0, dec.auth.bits_f2, dec.auth.bits_f3, true);
            break;
        case ROLE_WHEEL:
            log8(DREC_LAYOUT, ROLE_WHEEL, (uint8_t) dec.layout, dec.mismatch, wl.report_id, (uint8_t) wl.payload_bits,
                (uint8_t) (wl.payload_bits >> 8), (uint8_t) wl.button_count, wl.hat.present, idx, true);
            break;
        case ROLE_PEDALS:
            log8(DREC_LAYOUT, ROLE_PEDALS, (uint8_t) dec.layout, dec.mismatch, pl.report_id, (uint8_t) pl.payload_bits,
                (uint8_t) (pl.payload_bits >> 8), 0, 0, idx, true);
            break;
        default:
            break;
    }

    switch (dec.role) {
        case ROLE_AUTH:
            if (!g_auth_dev.present) {
                g_auth_dev = (hid_slot_t) { true, daddr, idx };
                auth_set_device_present(&g_auth, true, now_ms());
            }
            break;
        case ROLE_WHEEL:
            if (!g_wheel.present) {
                log_field(1, &wl.steer);
                log_field(2, &wl.hat);
                if (wl.button_count > 0) {
                    hid_field_t b1 = { true, wl.report_id, wl.button_ofs[0], 1, 0, 1, HID_FLAG_VARIABLE };
                    log_field(3, &b1);
                }
                g_input.wheel_layout = wl;
                g_input.wheel_layout_ok = true;
                g_input.wheel_valid = false;
                g_wheel = (hid_slot_t) { true, daddr, idx };
                tuh_hid_receive_report(daddr, idx);
#if CFG_FFB_ENABLED
                // The FFB output report of the same interface (as simagic-ff uses);
                // first action towards the base: stop every effect block.
                g_ffb_use_control = false;
                ffb_set_output_format(&g_ffb, desc, len);
                ffb_reset(&g_ffb, FFB_RESET_BOOT);
#endif
            }
            break;
        case ROLE_PEDALS:
            if (!g_pedals.present) {
                log_field(4, &pl.x);
                log_field(5, &pl.y);
                log_field(6, &pl.z);
                g_input.pedal_layout = pl;
                g_input.pedal_layout_ok = true;
                g_input.pedal_valid = false;
                g_pedals = (hid_slot_t) { true, daddr, idx };
                tuh_hid_receive_report(daddr, idx);
            }
            break;
        default:
            break;  // hub, passive, unknown, unconfirmed: enumerated, never polled or sent requests
    }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
    if (g_auth_dev.present && g_auth_dev.daddr == daddr && g_auth_dev.idx == idx) {
        g_auth_dev.present = false;
        auth_set_device_present(&g_auth, false, now_ms());
    }
    if (g_wheel.present && g_wheel.daddr == daddr && g_wheel.idx == idx) {
        g_wheel.present = false;
        input_wheel_lost(&g_input);  // steering centred, hat and buttons released
        memset(g_src_buttons, 0, sizeof(g_src_buttons));
        g_input_dirty = true;
        ffb_reset(&g_ffb, FFB_RESET_WHEEL_GONE);
    }
    if (g_pedals.present && g_pedals.daddr == daddr && g_pedals.idx == idx) {
        g_pedals.present = false;
        input_pedal_lost(&g_input);  // pedals released
        g_input_dirty = true;
    }
}

static void track_source_buttons(void) {
    uint16_t n = g_input.wheel_layout.button_count;
    for (uint16_t b = 1; b <= n && b <= WHEEL_MAX_BUTTONS; b++) {
        bool now = input_wheel_button(&g_input, b);
        uint8_t* byte = &g_src_buttons[(b - 1) / 8];
        uint8_t mask = (uint8_t) (1u << ((b - 1) % 8));
        bool before = (*byte & mask) != 0;
        if (now != before) {
            *byte = now ? (uint8_t) (*byte | mask) : (uint8_t) (*byte & ~mask);
#if ADAPTER_BENCH
            log8(DREC_SRC_BUTTON, (uint8_t) b, now, 0, 0, 0, 0, 0, 0, 0, false);
#endif
        }
    }
}

static void note_first_input(dev_role_t role, uint32_t count, const uint8_t* r, uint16_t len) {
    if (count == 1) {
        uint8_t d[8] = { (uint8_t) len, (uint8_t) (len >> 8), 0, 0, 0, 0, 0, 0 };
        for (uint16_t i = 0; i < 6 && i < len; i++) {
            d[2 + i] = r[i];
        }
        log_rec(DREC_FIRST_INPUT, (uint8_t) role, d, true);
    }
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx, uint8_t const* report, uint16_t len) {
    bool is_wheel = g_wheel.present && g_wheel.daddr == daddr && g_wheel.idx == idx;
    bool is_pedals = g_pedals.present && g_pedals.daddr == daddr && g_pedals.idx == idx;
    if (is_wheel || is_pedals) {
        bool ok = is_wheel ? input_wheel_report(&g_input, report, len) : input_pedal_report(&g_input, report, len);
        if (ok) {
            if (is_wheel) {
                note_first_input(ROLE_WHEEL, ++g_cnt.wheel_rx, report, len);
                track_source_buttons();
            } else {
                note_first_input(ROLE_PEDALS, ++g_cnt.pedal_rx, report, len);
            }
            g_input_dirty = true;
        } else if (len > 0) {
            g_cnt.input_rejects++;
            if (g_cnt.input_rejects <= 8 || (g_cnt.input_rejects & 0x3FF) == 0) {
                uint32_t rc = g_cnt.input_rejects;
                log8(DREC_INPUT_REJECT, is_wheel ? ROLE_WHEEL : ROLE_PEDALS, (uint8_t) len, (uint8_t) (len >> 8),
                    report[0], 0, (uint8_t) rc, (uint8_t) (rc >> 8), (uint8_t) (rc >> 16), (uint8_t) (rc >> 24), false);
            }
        }
        tuh_hid_receive_report(daddr, idx);
    }
}

void tuh_hid_get_report_complete_cb(uint8_t daddr, uint8_t idx, uint8_t report_id, uint8_t report_type, uint16_t len) {
    (void) report_type;
    if (g_auth_dev.present && g_auth_dev.daddr == daddr && g_auth_dev.idx == idx) {
        auth_complete(&g_auth, report_id, len, now_ms());
    }
}

void tuh_hid_set_report_complete_cb(uint8_t daddr, uint8_t idx, uint8_t report_id, uint8_t report_type, uint16_t len) {
    if (g_auth_dev.present && g_auth_dev.daddr == daddr && g_auth_dev.idx == idx) {
        auth_complete(&g_auth, report_id, len, now_ms());
    } else if (g_wheel.present && g_wheel.daddr == daddr && g_wheel.idx == idx && report_type == HID_REPORT_TYPE_OUTPUT) {
        ffb_packet_done(&g_ffb, len > 0);
    }
}

void tuh_hid_report_sent_cb(uint8_t daddr, uint8_t idx, uint8_t const* report, uint16_t len) {
    (void) report;
    if (g_wheel.present && g_wheel.daddr == daddr && g_wheel.idx == idx) {
        ffb_packet_done(&g_ffb, len > 0);
    }
}

// ------------------------------------------------------------------ TinyUSB device callbacks

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void) rhport;
    (void) in_isr;
    if (eventid == DCD_EVENT_BUS_RESET) {
        g_bus_resets++;
    }
}

void tud_mount_cb(void) {
    g_console_configured = true;
    g_input_dirty = true;
    memset(&g_last_sent, 0xFF, sizeof(g_last_sent));  // force a first report
    diag_push_u32(&g_diag, now_ms(), DREC_USB_DEV, 1, g_bus_resets, 0, true);
}

void tud_umount_cb(void) {
    g_console_configured = false;
    ffb_reset(&g_ffb, FFB_RESET_CONSOLE_GONE);
    diag_push_u32(&g_diag, now_ms(), DREC_USB_DEV, 2, g_bus_resets, 0, true);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    g_suspended = true;
    ffb_reset(&g_ffb, FFB_RESET_SUSPEND);
    diag_push_u32(&g_diag, now_ms(), DREC_USB_DEV, 3, g_bus_resets, remote_wakeup_en, true);
}

void tud_resume_cb(void) {
    g_suspended = false;
    g_input_dirty = true;
    diag_push_u32(&g_diag, now_ms(), DREC_USB_DEV, 4, g_bus_resets, 0, true);
}

static void note_console_req(uint8_t report_id, uint8_t type, bool set, uint16_t len) {
    uint16_t bit = (uint16_t) (report_id * 2u + (set ? 1u : 0u));
    uint8_t mask = (uint8_t) (1u << (bit & 7));
    if (!(g_console_req_seen[(bit >> 3) & 63] & mask)) {
        g_console_req_seen[(bit >> 3) & 63] |= mask;
        log8(DREC_CONSOLE_REQ, report_id, type, set, (uint8_t) len, (uint8_t) (len >> 8), 0, 0, 0, 0, true);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer,
    uint16_t reqlen) {
    (void) itf;
    uint32_t t = now_ms();
    note_console_req(report_id, (uint8_t) report_type, false, reqlen);
    // Dispatch on report ID only, as the reference adapter does.
    uint16_t n = auth_console_get_report(&g_auth, report_id, buffer, reqlen, t);
    if (n > 0) {
        return n;
    }
    memset(buffer, 0, reqlen);
    if (report_id == 0x03) {
        memcpy(buffer, g29_feature_0x03, reqlen < sizeof(g29_feature_0x03) ? reqlen : sizeof(g29_feature_0x03));
        return reqlen;
    }
    if (report_id == G29_INPUT_REPORT_ID && report_type == HID_REPORT_TYPE_INPUT) {
        memcpy(buffer, g_report.b, reqlen < G29_INPUT_PAYLOAD_LEN ? reqlen : G29_INPUT_PAYLOAD_LEN);
        return reqlen;
    }
    return reqlen;  // reference returns reqlen bytes for unknown IDs too (zeros here)
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer,
    uint16_t bufsize) {
    (void) itf;
    if (report_id != 0) {
        note_console_req(report_id, (uint8_t) report_type, true, bufsize);
    }
    if (auth_console_set_report(&g_auth, report_id, buffer, bufsize, now_ms())) {
        return;
    }
    // Everything else is console output (force feedback, LEDs). It is never forwarded
    // as-is: ffb.c translates steady forces into Simagic packets under the cap.
    g_cnt.console_out++;
#if CFG_FFB_ENABLED
    ffb_console_output(&g_ffb, report_id, buffer, bufsize, now_ms());
#endif
    if (g_cnt.console_out == 1) {
        uint8_t d[8] = { 0 };
        for (uint16_t i = 0; i < 8 && i < bufsize; i++) {
            d[i] = buffer[i];
        }
        log_rec(DREC_CONSOLE_OUTPUT, report_id, d, true);
    }
}

// ------------------------------------------------------------------ application tasks

static void auth_task(uint32_t t) {
    auth_request_t req;
    if (!auth_poll(&g_auth, t, &req)) {
        return;
    }
    bool ok;
    if (req.get) {
        ok = tuh_hid_get_report(g_auth_dev.daddr, g_auth_dev.idx, req.report_id, HID_REPORT_TYPE_FEATURE, req.buf, req.len);
    } else {
        ok = tuh_hid_set_report(g_auth_dev.daddr, g_auth_dev.idx, req.report_id, HID_REPORT_TYPE_FEATURE, req.buf, req.len);
    }
    auth_submitted(&g_auth, ok, t);
}

static void ffb_task(uint32_t t) {
#if CFG_FFB_ENABLED
    if (!g_wheel.present || !g_ffb.out_ok) {
        return;
    }
    uint8_t* payload = &g_ffb_ctrl_buf[1];
    uint8_t n = ffb_next_packet(&g_ffb, t, payload);
    if (n == 0) {
        return;
    }
    bool ok = false;
    if (!g_ffb_use_control) {
        if (!tuh_hid_send_ready(g_wheel.daddr, g_wheel.idx)) {
            ffb_packet_done(&g_ffb, false);  // endpoint busy: rebuilt from current state next loop
            return;
        }
        ok = tuh_hid_send_report(g_wheel.daddr, g_wheel.idx, g_ffb.out_report_id, payload, n);
        if (!ok) {
            // Ready but refused: the interface has no interrupt OUT endpoint.
            g_ffb_use_control = true;
            ffb_event(NULL, FFB_EV_OUT_MODE, 2, 0);
        }
    }
    if (g_ffb_use_control) {
        g_ffb_ctrl_buf[0] = g_ffb.out_report_id;
        ok = tuh_hid_set_report(g_wheel.daddr, g_wheel.idx, g_ffb.out_report_id, HID_REPORT_TYPE_OUTPUT,
            g_ffb.out_report_id ? g_ffb_ctrl_buf : payload, (uint16_t) (g_ffb.out_report_id ? n + 1 : n));
    }
    if (!ok) {
        ffb_packet_done(&g_ffb, false);
    }
#else
    (void) t;
#endif
}

static void report_task(uint32_t t) {
    if (g_hold_pending && (int32_t) (t - g_hold_next_ms) >= 0) {
        g_input_dirty = true;
    }
    if (g_input_dirty) {
        g_input_dirty = false;
        input_build_report(&g_input, &g_input_cfg, &g_report);
        g_hold_pending = input_apply_holds(&g_input, &g_input_cfg, &g_holds, &g_report, t, &g_hold_next_ms);
        bool ps = g29_get_button(&g_report, G29_BTN_PS);
        if (ps != g_ps_pressed) {
            g_ps_pressed = ps;
            log8(DREC_PS_BUTTON, ps, 6, 0, 0, 0, 0, 0, 0, 0, true);
        }
    }
    if (!g_console_configured || g_suspended || memcmp(&g_report, &g_last_sent, sizeof(g_report)) == 0) {
        return;
    }
    if (!tud_hid_ready()) {
        return;
    }
    if (tud_hid_report(G29_INPUT_REPORT_ID, g_report.b, G29_INPUT_PAYLOAD_LEN)) {
        g_last_sent = g_report;
        g_cnt.reports_sent++;
    } else {
        g_cnt.send_busy++;
    }
}

static void port_event_task(void) {
    for (uint8_t p = 1; p <= 2; p++) {
        uint32_t a = g_port_attach[p];
        uint32_t r = g_port_remove[p];
        if (a != g_port_attach_seen[p]) {
            g_port_attach_seen[p] = a;
            log8(DREC_PORT_EVENT, p, 1, 0, (uint8_t) a, (uint8_t) (a >> 8), (uint8_t) (a >> 16), (uint8_t) (a >> 24), 0, 0, true);
        }
        if (r != g_port_remove_seen[p]) {
            g_port_remove_seen[p] = r;
            log8(DREC_PORT_EVENT, p, 0, 0, (uint8_t) r, (uint8_t) (r >> 8), (uint8_t) (r >> 16), (uint8_t) (r >> 24), 0, 0, true);
        }
    }
}

static uint8_t heartbeat_flags(void) {
    uint8_t f = 0;
    if (g_console_configured) f |= HB_CONSOLE_CONFIGURED;
    if (g_auth_dev.present) f |= HB_AUTH_DEVICE_OK;
    if (g_input.wheel_valid) f |= HB_WHEEL_VALID;
    if (g_input.pedal_valid) f |= HB_PEDALS_VALID;
    if (g_port_attach[1] > g_port_remove[1]) f |= HB_PORT_A_ATTACHED;
    if (g_port_attach[2] > g_port_remove[2]) f |= HB_PORT_B_ATTACHED;
    if (g_suspended) f |= HB_SUSPENDED;
    return f;
}

static void heartbeat_task(uint32_t t) {
    static uint32_t last;
    if ((uint32_t) (t - last) < HEARTBEAT_MS) {
        return;
    }
    last = t;
    log_u16x4(DREC_HEARTBEAT, heartbeat_flags(), t / 1000, g_cnt.reports_sent, g_cnt.wheel_rx, g_cnt.pedal_rx, false);
    log_u16x4(DREC_HEARTBEAT2, (uint8_t) g_auth.ds, g_auth.jobs_ok, g_auth.delivered, g_auth.jobs_failed, g_cnt.console_out, false);
    log_u16x4(DREC_HEARTBEAT3, 0, g_diag.flash_dropped, g_diag.live_dropped, g_cnt.send_busy, g_cnt.input_rejects, false);
#if CFG_FFB_ENABLED
    log_u16x4(DREC_HEARTBEAT4, (uint8_t) (g_ffb.running | (g_ffb.out_ok << 1) | (g_ffb_use_control << 2)), g_ffb.rx_cmds,
        g_ffb.tx_packets, g_ffb.unsupported, g_ffb.max_gap_ms, false);
#endif
}

// LED: fast blink = not configured by a host; double blink = configured but no valid
// auth controller; slow blink = waiting for / running authentication; solid = the
// console received a complete signature since its last auth reset.
static void led_task(uint32_t t) {
    bool on;
    if (!g_console_configured) {
        on = (t / 125) % 2;
    } else if (!g_auth_dev.present) {
        uint32_t p = t % 1500;
        on = p < 120 || (p >= 240 && p < 360);
    } else if (g_auth_delivered_since_f3 && !g_auth_in_progress) {
        on = true;
    } else {
        on = (t / 500) % 2;
    }
    gpio_put(LED_PIN, on);
}

#if ADAPTER_BENCH
// ------------------------------------------------------------------ bench diagnostics (CDC)
// Line protocol, decoded by tools/adapter_monitor.py:
//   "L <32 hex>"  one diag record
//   "S <hex>"     live status snapshot (see bench_status_t)
// Input: "BOOTSEL" line, or setting 1200 baud, reboots into the USB bootloader.

typedef struct __attribute__((packed)) {
    uint32_t t_ms;
    uint8_t flags;
    uint8_t auth_state;
    uint8_t valid;  // bit0 steer, 1 hat, 2 throttle, 3 brake, 4 clutch
    uint8_t reserved;
    int32_t steer, hat, throttle, brake, clutch;
    uint8_t src_buttons[WHEEL_MAX_BUTTONS / 8];
    uint8_t g29_buttons[3];  // payload bytes 4..6
    uint8_t g29_btn3;        // payload byte 53
    uint16_t g29_wheel, g29_throttle, g29_brake, g29_clutch;
    uint32_t reports_sent, wheel_rx, pedal_rx;
    uint32_t auth_jobs_ok, auth_delivered, auth_jobs_failed;
    int16_t ffb_mag;   // last magnitude sent to the wheelbase (-10000..10000)
    uint8_t ffb_flags; // bit0 running, bit1 output format ok, bit2 control-transfer mode, bit3 enabled
    uint8_t ffb_pct;   // cap
    uint32_t ffb_rx;   // console FFB commands
} bench_status_t;

static const char hexdig[] = "0123456789abcdef";

static uint32_t hex_line(char* out, char tag, const uint8_t* p, uint32_t n) {
    uint32_t k = 0;
    out[k++] = tag;
    out[k++] = ' ';
    for (uint32_t i = 0; i < n; i++) {
        out[k++] = hexdig[p[i] >> 4];
        out[k++] = hexdig[p[i] & 15];
    }
    out[k++] = '\n';
    return k;
}

static void bench_task(uint32_t t) {
    static uint32_t last_status;
    static char rx_line[24];
    static uint8_t rx_len;
    char line[2 + 2 * sizeof(bench_status_t) + 2];

    while (tud_cdc_available()) {
        int c = tud_cdc_read_char();
        if (c == '\n' || c == '\r') {
            rx_line[rx_len] = 0;
            if (strcmp(rx_line, "BOOTSEL") == 0) {
                reset_usb_boot(0, 0);
            } else if (strncmp(rx_line, "FFBTEST ", 8) == 0) {
                // Raw magnitude -10000..10000 before the cap, 600 ms, invert not applied.
                int v = atoi(&rx_line[8]);
                ffb_test(&g_ffb, (int16_t) (v > 10000 ? 10000 : v < -10000 ? -10000 : v), 600, t);
                log8(DREC_FFB, 100, (uint8_t) v, (uint8_t) ((uint16_t) v >> 8), 0, 0, 0, 0, 0, 0, true);
            }
            rx_len = 0;
        } else if (rx_len < sizeof(rx_line) - 1) {
            rx_line[rx_len++] = (char) c;
        }
    }

    // Records stay in the RAM ring until a terminal has the port open (DTR set);
    // TinyUSB's CDC FIFO overwrites itself while nobody is reading.
    diag_rec_t r;
    while (tud_cdc_connected() && tud_cdc_write_available() >= 2 + 32 + 1 && diag_live_peek(&g_diag, &r)) {
        uint32_t n = hex_line(line, 'L', (const uint8_t*) &r, sizeof(r));
        tud_cdc_write(line, n);
        diag_live_consume(&g_diag);
    }

    if ((uint32_t) (t - last_status) >= 50 && tud_cdc_write_available() >= (int) (2 + 2 * sizeof(bench_status_t) + 1)) {
        last_status = t;
        bench_status_t s;
        memset(&s, 0, sizeof(s));
        s.t_ms = t;
        s.flags = heartbeat_flags();
        s.auth_state = (uint8_t) g_auth.ds;
        // Locals, not &s.field: the struct is packed and the M0+ faults on unaligned words.
        int32_t steer = 0, hat = 0, thr = 0, brk = 0, clu = 0;
        if (input_raw_steer(&g_input, &steer)) s.valid |= 0x01;
        if (input_raw_hat(&g_input, &hat)) s.valid |= 0x02;
        if (input_raw_throttle(&g_input, &thr)) s.valid |= 0x04;
        if (input_raw_brake(&g_input, &brk)) s.valid |= 0x08;
        if (input_raw_clutch(&g_input, &clu)) s.valid |= 0x10;
        s.steer = steer;
        s.hat = hat;
        s.throttle = thr;
        s.brake = brk;
        s.clutch = clu;
        memcpy(s.src_buttons, g_src_buttons, sizeof(s.src_buttons));
        memcpy(s.g29_buttons, &g_report.b[G29_OFS_BTN0], 3);
        s.g29_btn3 = g_report.b[G29_OFS_BTN3];
        s.g29_wheel = g29_get_u16(&g_report, G29_OFS_WHEEL);
        s.g29_throttle = g29_get_u16(&g_report, G29_OFS_THROTTLE);
        s.g29_brake = g29_get_u16(&g_report, G29_OFS_BRAKE);
        s.g29_clutch = g29_get_u16(&g_report, G29_OFS_CLUTCH);
        s.reports_sent = g_cnt.reports_sent;
        s.wheel_rx = g_cnt.wheel_rx;
        s.pedal_rx = g_cnt.pedal_rx;
        s.auth_jobs_ok = g_auth.jobs_ok;
        s.auth_delivered = g_auth.delivered;
        s.auth_jobs_failed = g_auth.jobs_failed;
        s.ffb_mag = g_ffb.sent_mag;
        s.ffb_flags = (uint8_t) (g_ffb.running | (g_ffb.out_ok << 1) | (g_ffb_use_control << 2) | (CFG_FFB_ENABLED << 3));
        s.ffb_pct = g_ffb.strength_percent;
        s.ffb_rx = g_ffb.rx_cmds;
        uint32_t n = hex_line(line, 'S', (const uint8_t*) &s, sizeof(s));
        tud_cdc_write(line, n);
    }
    tud_cdc_write_flush();
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding) {
    (void) itf;
    if (coding->bit_rate == 1200) {
        reset_usb_boot(0, 0);
    }
}
#endif

// ------------------------------------------------------------------ main

int main(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    diag_init(&g_diag);
    g_diag.live_enabled = ADAPTER_BENCH;
    auth_init(&g_auth, auth_event, NULL);
    ffb_init(&g_ffb, CFG_FFB_STRENGTH_PERCENT, CFG_FFB_INVERT, CFG_FFB_STALE_MS, ffb_event, NULL);
    g29_report_neutral(&g_report);
    memset(&g_last_sent, 0xFF, sizeof(g_last_sent));

    // Prepare this boot's flash log slot before USB starts (erasing takes ~0.6 s).
    uint8_t rr = reset_reason();
    uint16_t clk_mhz = (uint16_t) (clock_get_hz(clk_sys) / 1000000u);
    bool flash_ok = ((uintptr_t) &__flash_binary_end - XIP_BASE) <= DIAG_FLASH_REGION_OFFSET &&
                    diag_flash_open(&g_diag, &g_flash_ops, FW_VERSION, ADAPTER_BENCH, rr, clk_mhz);
    log8(DREC_BOOT, ADAPTER_BENCH, (uint8_t) g_diag.boot_counter, (uint8_t) (g_diag.boot_counter >> 8),
        (uint8_t) (g_diag.boot_counter >> 16), (uint8_t) (g_diag.boot_counter >> 24), rr, (uint8_t) clk_mhz,
        (uint8_t) FW_VERSION, (uint8_t) (FW_VERSION >> 8), true);
    if (!flash_ok) {
        log8(DREC_ERROR, 1, 0, 0, 0, 0, 0, 0, 0, 0, true);
    }

    // Port A (GP0/GP1) is the default root port; port B is added after host init on
    // the same PIO state machines (Pico-PIO-USB discussion 72, jfedor2 r2026-11-07).
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = 0;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tusb_init();
    int add = pio_usb_host_add_port(PORT_B_DP_PIN, PIO_USB_PINOUT_DPDM);
    if (add != 0) {
        log8(DREC_ERROR, 2, (uint8_t) add, 0, 0, 0, 0, 0, 0, 0, true);
    }

    watchdog_enable(WATCHDOG_MS, true);

    while (true) {
        tuh_task();
        tud_task();
        uint32_t t = now_ms();
        auth_task(t);
        report_task(t);
        ffb_task(t);
        port_event_task();
        heartbeat_task(t);
        led_task(t);
#if ADAPTER_BENCH
        bench_task(t);
#endif
        diag_flash_service(&g_diag, t);
        watchdog_update();
    }
}
