// Diagnostic event log (pure C core).
//
// Records go into a bounded RAM ring. Two independent consumers read it:
//   - the flash sink persists records into a per-boot slot of the Pico's flash,
//     so a console session survives the power loss when the adapter is unplugged
//     from the PS5 (read back in BOOTSEL mode with picotool: no new boot happens);
//   - the live sink (bench build only) streams them over a CDC serial port.
// A slow consumer loses the oldest records (counted), never blocks the producer.
#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t t_ms;
    uint16_t seq;
    uint8_t type;
    uint8_t a;
    uint8_t d[8];
} diag_rec_t;

_Static_assert(sizeof(diag_rec_t) == 16, "diag record must be 16 bytes");

// Keep in sync with tools/adapter_log.py.
typedef enum {
    DREC_BOOT = 1,         // a: variant, d0-3 boot counter, d4 reset reason, d5 clk MHz, d6-7 fw version
    DREC_PORT_EVENT,       // a: rhport (1 = port A, 2 = port B), d0 1=attach 0=remove, d2-5 event count on that root port (incl. hub children)
    DREC_DEV_MOUNT,        // a: daddr, d0 rhport, d1 hub addr, d2 hub port, d3 speed, d4-5 vid, d6-7 pid
    DREC_DEV_ROLE,         // a: daddr, d0 role, d1 topology ok, d2 expected rhport, d3 expected behind hub
    DREC_DEV_UMOUNT,       // a: daddr, d0 role
    DREC_HID_MOUNT,        // a: daddr, d0 idx, d1 itf num, d2-3 desc len, d4-7 desc CRC32
    DREC_HID_DESC_CHUNK,   // a: daddr, d0 idx, d1 chunk, d2-7 six descriptor bytes
    DREC_LAYOUT,           // a: role, d0 status, d1 mismatch bits, d2 report id, d3-4 payload bits, d5 buttons, d6 hat, d7 hid idx
    DREC_LAYOUT_FIELD,     // a: field id, d0-1 bit offset, d2 bit size, d3 report id, d4-5 lmin (s16 sat), d6-7 lmax (u16 sat)
    DREC_AUTH_DESC,        // a: daddr, d0 ok, d1 idx, d2-3 F0 bits, d4-5 F2 bits, d6-7 F3 bits
    DREC_AUTH,             // a: auth event, d0 event arg, d1-4 event value, d5 ds state
    DREC_FIRST_INPUT,      // a: role, d0-1 len, d2.. first bytes
    DREC_INPUT_REJECT,     // a: role, d0-1 len, d2 first byte, d4-7 reject count
    DREC_USB_DEV,          // a: event (1 configured, 2 unconfigured, 3 suspend, 4 resume), d0-3 bus resets
    DREC_CONSOLE_REQ,      // first request per report id: a: report id, d0 type, d1 1=SET 0=GET, d2-3 len
    DREC_CONSOLE_OUTPUT,   // first console output report: a: report id, d0-7 first data bytes
    DREC_PS_BUTTON,        // a: 1 pressed / 0 released, d0 source button
    DREC_SRC_BUTTON,       // a: source button, d0 pressed
    DREC_HEARTBEAT,        // a: flags, d0-1 uptime s, d2-3 reports sent, d4-5 wheel rx, d6-7 pedal rx (u16 wrap)
    DREC_HEARTBEAT2,       // a: auth state, d0-1 jobs ok, d2-3 delivered, d4-5 jobs failed, d6-7 console out reports
    DREC_HEARTBEAT3,       // a: 0, d0-1 flash dropped, d2-3 live dropped, d4-5 report send busy, d6-7 input rejects
    DREC_ERROR,            // a: code, d0-7 detail
    DREC_LOG_FULL,         // flash slot full; last record of a slot
    DREC_FFB,              // a: ffb_event_t (100 = bench test), d0 arg, d1-4 value
    DREC_HEARTBEAT4,       // a: bit0 running bit1 format ok bit2 control mode, d0-1 console cmds, d2-3 packets sent, d4-5 unsupported, d6-7 max gap ms
    DREC_HEARTBEAT5,       // window: d0 max |console level| (0..127), d1 avg |level|, d2-3 max |sent magnitude|, d4-5 starts, d6-7 tx failed
} diag_type_t;

// Heartbeat flag bits.
#define HB_CONSOLE_CONFIGURED 0x01
#define HB_AUTH_DEVICE_OK 0x02
#define HB_WHEEL_VALID 0x04
#define HB_PEDALS_VALID 0x08
#define HB_PORT_A_ATTACHED 0x10
#define HB_PORT_B_ATTACHED 0x20
#define HB_SUSPENDED 0x40

// Flash geometry (offsets from the start of flash).
#define DIAG_FLASH_PAGE 256u
#define DIAG_FLASH_REGION_OFFSET 0x100000u  // last 1 MiB of the Pico's 2 MiB flash
#define DIAG_FLASH_SLOT_SIZE 0x40000u        // 256 KiB per boot: >16000 records
#define DIAG_FLASH_SLOTS 4u
#define DIAG_SLOT_MAGIC 0x4C354750u          // "PG5L"
#define DIAG_SLOT_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;
    uint32_t boot_counter;
    uint32_t fw_version;
    uint8_t variant;  // 0 console build, 1 bench build
    uint8_t reset_reason;
    uint16_t clk_mhz;
    uint32_t slot_size;
    uint32_t header_crc;  // CRC32 of the preceding bytes
} diag_slot_header_t;

typedef struct {
    bool (*read)(void* ctx, uint32_t offset, void* buf, uint32_t len);
    bool (*erase)(void* ctx, uint32_t offset, uint32_t len);          // slot-aligned
    bool (*program)(void* ctx, uint32_t offset, const void* buf, uint32_t len);  // one 256-byte page
    void* ctx;
} diag_flash_ops_t;

#define DIAG_RING 512u

typedef struct {
    diag_rec_t ring[DIAG_RING];
    uint32_t head;  // records pushed so far
    uint16_t seq;

    // flash sink
    const diag_flash_ops_t* ops;
    bool flash_active;
    uint8_t slot;
    uint32_t boot_counter;
    uint32_t slot_base;
    uint32_t flash_tail;
    uint32_t flash_wpos;  // byte offset of the next record inside the slot
    uint32_t flash_dropped;
    uint32_t last_flush_ms;
    bool urgent;
    uint32_t urgent_since_ms;
    uint32_t flush_interval_ms;
    uint32_t urgent_delay_ms;

    // live sink
    bool live_enabled;
    uint32_t live_tail;
    uint32_t live_dropped;
} diag_t;

void diag_init(diag_t* d);

// Chooses the slot after the newest valid one, erases it and writes its header.
// Returns false (flash sink disabled) on any flash error.
bool diag_flash_open(diag_t* d, const diag_flash_ops_t* ops, uint32_t fw_version, uint8_t variant,
    uint8_t reset_reason, uint16_t clk_mhz);

void diag_push(diag_t* d, uint32_t now_ms, uint8_t type, uint8_t a, const uint8_t* data8, bool urgent);
void diag_push_u32(diag_t* d, uint32_t now_ms, uint8_t type, uint8_t a, uint32_t v0, uint32_t v1, bool urgent);

// Writes at most one flash page if a flush is due. Returns true if it programmed.
bool diag_flash_service(diag_t* d, uint32_t now_ms);

// Live sink: next record not yet sent (false if none).
bool diag_live_peek(const diag_t* d, diag_rec_t* out);
void diag_live_consume(diag_t* d);

#endif
