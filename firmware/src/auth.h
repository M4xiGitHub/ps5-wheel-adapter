// PS4/PS5 controller-authentication bridge (pure state machine, no SDK calls).
//
// Console side (G29 HID feature reports, answered synchronously from tud callbacks):
//   GET 0xF3  reset / page sizes        -> fixed reply, as the reference adapter
//   SET 0xF0  nonce page 0..4 (63 B)    -> stored verbatim
//   GET 0xF2  signing status            -> 0x10 busy / 0x00 ready (+CRC32)
//   GET 0xF1  signature page 0..18      -> verbatim page from the auth controller
// Downstream (one control transfer at a time to the validated auth controller):
//   GET F3, SET F0 x5 (console pages verbatim), GET F2 until ready, GET F1 x19.
//
// The protocol sequence follows jfedor2/wheel-adapter (MIT). Differences are listed
// in docs/CONSOLE_BEHAVIOR.md.
#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stdint.h>

#define AUTH_NONCE_PAGES 5
#define AUTH_SIG_PAGES 19
#define AUTH_PAGE_LEN 63  // feature payload after the report ID
#define AUTH_STATUS_BUSY 0x10
#define AUTH_STATUS_READY 0x00

typedef enum {
    AUTH_DS_IDLE = 0,     // no job
    AUTH_DS_WAIT_DEVICE,  // job ready, no validated auth controller mounted
    AUTH_DS_RESET,        // GET F3
    AUTH_DS_NONCE,        // SET F0 page ds_page
    AUTH_DS_POLL,         // GET F2 until ready
    AUTH_DS_SIG,          // GET F1 page ds_page
    AUTH_DS_READY,        // signature buffered, serving console
    AUTH_DS_FAILED,       // gave up on this job; console will retry
} auth_ds_state_t;

typedef enum {
    AUTH_EV_CONSOLE_F3 = 1,     // a: 0
    AUTH_EV_CONSOLE_NONCE_PAGE, // a: page, b: nonce_id | (crc_ok << 8) | (len << 16)
    AUTH_EV_CONSOLE_NONCE_BAD,  // a: reason (auth_bad_t), b: page | nonce_id << 8 | len << 16
    AUTH_EV_JOB_START,          // a: nonce_id, b: job number
    AUTH_EV_JOB_CANCELLED,      // a: state it was in, b: job number
    AUTH_EV_DS_STEP_OK,         // a: report id, b: page | polls << 8
    AUTH_EV_DS_STEP_FAIL,       // a: report id, b: retries | len << 8
    AUTH_EV_DS_SUBMIT_REJECTED, // a: report id, b: count
    AUTH_EV_DS_SIGNED,          // a: polls (saturated), b: ms spent signing
    AUTH_EV_DS_READY,           // a: 0, b: ms since job start
    AUTH_EV_DS_PAGE_CHECK,      // a: page, b: bit0 crc_ok, bit1 id_ok, bit2 page_ok, bits 8-15 page field
    AUTH_EV_DS_FAILED,          // a: report id of last step, b: reason
    AUTH_EV_DS_XFER_TIMEOUT,    // a: report id, b: ms
    AUTH_EV_DS_STALE_COMPLETE,  // a: report id, b: 0
    AUTH_EV_CONSOLE_F2,         // a: status returned, b: number of F2 queries in this job
    AUTH_EV_CONSOLE_F1_PAGE,    // a: page served, b: ready
    AUTH_EV_DELIVERED,          // a: nonce_id, b: total deliveries
    AUTH_EV_DEVICE_PRESENT,     // a: present
} auth_event_t;

typedef enum {
    AUTH_BAD_LEN = 1,
    AUTH_BAD_PAGE_RANGE = 2,
    AUTH_BAD_SEQUENCE = 3,
} auth_bad_t;

typedef void (*auth_event_cb_t)(void* ctx, auth_event_t ev, uint8_t a, uint32_t b);

typedef struct {
    bool get;           // true: GET_REPORT, false: SET_REPORT
    uint8_t report_id;
    uint8_t* buf;       // report ID at buf[0]; must stay untouched until completion
    uint16_t len;       // including report ID
} auth_request_t;

typedef struct {
    // configuration
    uint32_t xfer_timeout_ms;  // backstop above the USB stack's own control timeout
    uint32_t sign_timeout_ms;  // give up polling F2 after this long
    uint32_t retry_delay_ms;   // pause after a failed or rejected submission
    uint8_t max_retries;       // per step
    auth_event_cb_t ev;
    void* ev_ctx;

    // console side
    uint8_t nonce_id;
    uint8_t nonce_pages[AUTH_NONCE_PAGES][AUTH_PAGE_LEN];
    uint8_t pages_rx;
    bool collecting;
    uint32_t job;
    uint32_t f2_queries;
    uint8_t last_f2_status;

    // downstream
    bool device_present;
    auth_ds_state_t ds;
    uint8_t ds_page;
    uint8_t retries;
    bool xfer_outstanding;
    uint8_t xfer_report_id;
    uint32_t xfer_job;
    uint32_t xfer_started_ms;
    uint32_t next_submit_ms;
    uint32_t job_started_ms;
    uint32_t sign_started_ms;
    uint32_t polls;
    uint32_t rejected;
    uint8_t xbuf[64];

    // result
    uint8_t sig_pages[AUTH_SIG_PAGES][AUTH_PAGE_LEN];
    bool sig_ready;
    uint8_t serve_page;

    // statistics
    uint32_t jobs_ok;
    uint32_t jobs_failed;
    uint32_t delivered;
} auth_t;

void auth_init(auth_t* a, auth_event_cb_t ev, void* ev_ctx);

// The validated auth controller was mounted (true) or removed (false).
void auth_set_device_present(auth_t* a, bool present, uint32_t now_ms);

// Console SET_REPORT(feature). buf excludes the report ID. Returns true if consumed.
bool auth_console_set_report(auth_t* a, uint8_t report_id, const uint8_t* buf, uint16_t len, uint32_t now_ms);

// Console GET_REPORT(feature). Fills reqlen bytes (report ID excluded) and returns
// the length to send, or 0 if the report ID is not an auth report.
uint16_t auth_console_get_report(auth_t* a, uint8_t report_id, uint8_t* buf, uint16_t reqlen, uint32_t now_ms);

// Returns true and fills req when a downstream transfer should be submitted now.
bool auth_poll(auth_t* a, uint32_t now_ms, auth_request_t* req);

// Result of handing the request from auth_poll() to the USB host stack.
void auth_submitted(auth_t* a, bool accepted, uint32_t now_ms);

// Completion of the outstanding downstream transfer. len is 0 on failure.
void auth_complete(auth_t* a, uint8_t report_id, uint16_t len, uint32_t now_ms);

const char* auth_state_name(auth_ds_state_t s);

#endif
