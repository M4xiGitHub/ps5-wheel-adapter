// Authentication bridge: sequencing, verbatim forwarding, single control-transfer
// ownership, retries, timeouts, cancellation and device loss.
#include "auth.h"
#include "crc32.h"
#include "test_util.h"

// ---------------------------------------------------------------- event capture
static int ev_count[32];
static void on_event(void* ctx, auth_event_t ev, uint8_t a, uint32_t b) {
    (void) ctx;
    (void) a;
    (void) b;
    if ((int) ev < 32) {
        ev_count[ev]++;
    }
}

// ---------------------------------------------------------------- simulated console
static void console_page(uint8_t* page, uint8_t nonce_id, uint8_t n) {
    page[0] = nonce_id;
    page[1] = n;
    page[2] = 0;
    for (int i = 0; i < 56; i++) {
        page[3 + i] = (uint8_t) (n * 56 + i);
    }
    uint8_t id = 0xF0;
    uint32_t crc = crc32_update(crc32_update(0, &id, 1), page, 59);
    page[59] = (uint8_t) crc;
    page[60] = (uint8_t) (crc >> 8);
    page[61] = (uint8_t) (crc >> 16);
    page[62] = (uint8_t) (crc >> 24);
}

static void console_send_nonce(auth_t* a, uint8_t nonce_id, uint32_t t) {
    for (uint8_t n = 0; n < AUTH_NONCE_PAGES; n++) {
        uint8_t page[63];
        console_page(page, nonce_id, n);
        CHECK(auth_console_set_report(a, 0xF0, page, 63, t));
    }
}

// ---------------------------------------------------------------- simulated Hori
typedef struct {
    uint8_t nonce[AUTH_NONCE_PAGES][63];
    int pages_rx;
    int busy_polls;  // F2 answers "busy" this many times
    int f2_count;
    int f1_count;
    uint8_t nonce_id;
} hori_t;

static void hori_sig_page(const hori_t* h, uint8_t n, uint8_t* out64) {
    out64[0] = 0xF1;
    out64[1] = h->nonce_id;
    out64[2] = n;
    out64[3] = 0;
    for (int i = 0; i < 56; i++) {
        out64[4 + i] = (uint8_t) (0xA0 ^ (n * 7 + i));
    }
    uint32_t crc = crc32_calc(out64, 60);
    out64[60] = (uint8_t) crc;
    out64[61] = (uint8_t) (crc >> 8);
    out64[62] = (uint8_t) (crc >> 16);
    out64[63] = (uint8_t) (crc >> 24);
}

// Executes one request the way the USB host stack would, returning the completion length.
static uint16_t hori_execute(hori_t* h, const auth_request_t* r) {
    if (!r->get) {
        CHECK_EQ(r->report_id, 0xF0);
        CHECK_EQ(r->len, 64);
        CHECK_EQ(r->buf[0], 0xF0);
        if (h->pages_rx < AUTH_NONCE_PAGES) {
            memcpy(h->nonce[h->pages_rx], &r->buf[1], 63);
            h->nonce_id = r->buf[1];
            h->pages_rx++;
        }
        return r->len;
    }
    memset(r->buf, 0, r->len);
    r->buf[0] = r->report_id;
    switch (r->report_id) {
        case 0xF3:
            CHECK_EQ(r->len, 8);
            r->buf[2] = 0x38;
            r->buf[3] = 0x38;
            h->pages_rx = 0;
            h->f1_count = 0;
            return r->len;
        case 0xF2:
            CHECK_EQ(r->len, 16);
            r->buf[1] = h->nonce_id;
            r->buf[2] = (h->f2_count++ < h->busy_polls) ? 0x10 : 0x00;
            return r->len;
        case 0xF1:
            CHECK_EQ(r->len, 64);
            hori_sig_page(h, (uint8_t) h->f1_count++, r->buf);
            return r->len;
    }
    return 0;
}

// Pumps the bridge until it has nothing more to submit (max steps).
static int pump(auth_t* a, hori_t* h, uint32_t* t, int max_steps) {
    int steps = 0;
    for (int i = 0; i < max_steps; i++) {
        auth_request_t r;
        if (!auth_poll(a, *t, &r)) {
            *t += 1;
            if (!a->xfer_outstanding && (a->ds == AUTH_DS_READY || a->ds == AUTH_DS_FAILED || a->ds == AUTH_DS_IDLE ||
                                            a->ds == AUTH_DS_WAIT_DEVICE)) {
                break;
            }
            continue;
        }
        auth_submitted(a, true, *t);
        auth_request_t again;
        CHECK(!auth_poll(a, *t, &again));  // one control transfer at a time
        uint16_t len = hori_execute(h, &r);
        *t += 2;
        auth_complete(a, r.report_id, len, *t);
        steps++;
    }
    return steps;
}

static void reset_events(void) {
    memset(ev_count, 0, sizeof(ev_count));
}

// ---------------------------------------------------------------- tests

static void test_happy_path_verbatim(void) {
    auth_t a;
    hori_t h = { .busy_polls = 3 };
    uint32_t t = 1000;
    reset_events();
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);

    uint8_t buf[63];
    CHECK_EQ(auth_console_get_report(&a, 0xF3, buf, 7, t), 7);
    CHECK_EQ(buf[0], 0x00);
    CHECK_EQ(buf[1], 0x38);
    CHECK_EQ(buf[2], 0x38);

    console_send_nonce(&a, 0x42, t);
    CHECK_EQ(a.ds, AUTH_DS_RESET);
    CHECK_EQ(ev_count[AUTH_EV_JOB_START], 1);

    // Busy until the downstream signature is complete.
    CHECK_EQ(auth_console_get_report(&a, 0xF2, buf, 15, t), 15);
    CHECK_EQ(buf[0], 0x42);
    CHECK_EQ(buf[1], AUTH_STATUS_BUSY);

    int steps = pump(&a, &h, &t, 200);
    CHECK_EQ(steps, 1 + 5 + 4 + 19);  // F3, 5x F0, 3 busy + 1 ready F2, 19x F1
    CHECK_EQ(a.ds, AUTH_DS_READY);
    // Hori received the console's pages byte for byte (CRC included).
    for (uint8_t n = 0; n < AUTH_NONCE_PAGES; n++) {
        uint8_t page[63];
        console_page(page, 0x42, n);
        CHECK(memcmp(h.nonce[n], page, 63) == 0);
    }
    CHECK_EQ(ev_count[AUTH_EV_DS_SIGNED], 1);

    CHECK_EQ(auth_console_get_report(&a, 0xF2, buf, 15, t), 15);
    CHECK_EQ(buf[1], AUTH_STATUS_READY);
    uint8_t id = 0xF2;
    uint32_t crc = crc32_update(crc32_update(0, &id, 1), buf, 11);
    CHECK_EQ(buf[11] | (buf[12] << 8) | (buf[13] << 16) | ((uint32_t) buf[14] << 24), crc);

    // Console reads 19 pages; each equals the Hori's page minus its report ID.
    for (uint8_t n = 0; n < AUTH_SIG_PAGES; n++) {
        uint8_t expect[64];
        hori_sig_page(&h, n, expect);
        CHECK_EQ(auth_console_get_report(&a, 0xF1, buf, 63, t), 63);
        CHECK(memcmp(buf, &expect[1], 63) == 0);
    }
    CHECK_EQ(ev_count[AUTH_EV_DELIVERED], 1);
    CHECK_EQ(a.delivered, 1);
    CHECK_EQ(a.jobs_ok, 1);
    CHECK_EQ(ev_count[AUTH_EV_DS_PAGE_CHECK], 1);  // page 0 always logged, no CRC complaints

    // A second round works the same way after the console resets with F3.
    auth_console_get_report(&a, 0xF3, buf, 7, t);
    CHECK_EQ(auth_console_get_report(&a, 0xF2, buf, 15, t), 15);
    CHECK_EQ(buf[1], AUTH_STATUS_BUSY);
    h.f2_count = 0;
    console_send_nonce(&a, 0x43, t);
    pump(&a, &h, &t, 200);
    CHECK_EQ(a.ds, AUTH_DS_READY);
    for (uint8_t n = 0; n < AUTH_SIG_PAGES; n++) {
        auth_console_get_report(&a, 0xF1, buf, 63, t);
    }
    CHECK_EQ(a.delivered, 2);
}

static void test_waits_for_device_and_survives_loss(void) {
    auth_t a;
    hori_t h = { .busy_polls = 0 };
    uint32_t t = 0;
    auth_init(&a, on_event, NULL);
    console_send_nonce(&a, 7, t);
    CHECK_EQ(a.ds, AUTH_DS_WAIT_DEVICE);
    auth_request_t r;
    CHECK(!auth_poll(&a, t, &r));  // nothing goes anywhere without the validated device

    auth_set_device_present(&a, true, t);
    CHECK_EQ(a.ds, AUTH_DS_RESET);
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, true, t);
    // Device unplugged with a transfer outstanding: the stack drops it.
    auth_set_device_present(&a, false, t + 1);
    CHECK(!a.xfer_outstanding);
    CHECK_EQ(a.ds, AUTH_DS_WAIT_DEVICE);
    auth_complete(&a, 0xF3, 0, t + 2);  // late failure callback is ignored as stale
    CHECK_EQ(a.ds, AUTH_DS_WAIT_DEVICE);
    auth_set_device_present(&a, true, t + 3);
    pump(&a, &h, &t, 100);
    CHECK_EQ(a.ds, AUTH_DS_READY);
}

static void test_rejected_submission_retries(void) {
    auth_t a;
    uint32_t t = 0;
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    console_send_nonce(&a, 1, t);
    auth_request_t r;
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, false, t);  // control pipe queue full / device busy
    CHECK(!a.xfer_outstanding);
    CHECK_EQ(a.ds, AUTH_DS_RESET);
    CHECK(!auth_poll(&a, t + 1, &r));  // backs off
    CHECK(auth_poll(&a, t + a.retry_delay_ms, &r));
    CHECK_EQ(r.report_id, 0xF3);
    CHECK_EQ(a.rejected, 1);
}

static void test_step_failures_then_give_up(void) {
    auth_t a;
    uint32_t t = 0;
    reset_events();
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    console_send_nonce(&a, 1, t);
    for (int i = 0; i <= a.max_retries; i++) {
        auth_request_t r;
        t += 10;
        CHECK(auth_poll(&a, t, &r));
        auth_submitted(&a, true, t);
        auth_complete(&a, r.report_id, 0, t + 1);  // STALL / error
    }
    CHECK_EQ(a.ds, AUTH_DS_FAILED);
    CHECK_EQ(a.jobs_failed, 1);
    CHECK_EQ(ev_count[AUTH_EV_DS_FAILED], 1);
    uint8_t buf[15];
    auth_console_get_report(&a, 0xF2, buf, 15, t);
    CHECK_EQ(buf[1], AUTH_STATUS_BUSY);  // console keeps waiting and restarts on its own
    auth_request_t r;
    CHECK(!auth_poll(&a, t + 100, &r));
    // The next console round starts a fresh job.
    hori_t h = { .busy_polls = 1 };
    console_send_nonce(&a, 2, t);
    pump(&a, &h, &t, 100);
    CHECK_EQ(a.ds, AUTH_DS_READY);
}

static void test_sign_timeout(void) {
    auth_t a;
    hori_t h = { .busy_polls = 1000000 };
    uint32_t t = 0;
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    console_send_nonce(&a, 3, t);
    for (int i = 0; i < 20000 && a.ds != AUTH_DS_FAILED; i++) {
        auth_request_t r;
        if (auth_poll(&a, t, &r)) {
            auth_submitted(&a, true, t);
            auth_complete(&a, r.report_id, hori_execute(&h, &r), t + 1);
        }
        t += 2;
    }
    CHECK_EQ(a.ds, AUTH_DS_FAILED);
    CHECK(t >= a.sign_timeout_ms);
}

static void test_new_nonce_cancels_and_stale_completion_ignored(void) {
    auth_t a;
    hori_t h = { .busy_polls = 0 };
    uint32_t t = 0;
    reset_events();
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    console_send_nonce(&a, 10, t);
    auth_request_t r;
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, true, t);
    uint8_t page[63];
    console_page(page, 11, 0);  // console starts over mid-transfer
    auth_console_set_report(&a, 0xF0, page, 63, t);
    CHECK_EQ(ev_count[AUTH_EV_JOB_CANCELLED], 1);
    auth_request_t r2;
    CHECK(!auth_poll(&a, t, &r2));  // buffer still owned by the old transfer
    auth_complete(&a, r.report_id, r.len, t + 1);
    CHECK_EQ(ev_count[AUTH_EV_DS_STALE_COMPLETE], 1);
    for (uint8_t n = 1; n < AUTH_NONCE_PAGES; n++) {
        console_page(page, 11, n);
        auth_console_set_report(&a, 0xF0, page, 63, t);
    }
    pump(&a, &h, &t, 100);
    CHECK_EQ(a.ds, AUTH_DS_READY);
    CHECK_EQ(h.nonce_id, 11);
}

static void test_bad_console_pages(void) {
    auth_t a;
    uint32_t t = 0;
    reset_events();
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    uint8_t page[63];
    console_page(page, 5, 1);  // page 1 before page 0
    CHECK(auth_console_set_report(&a, 0xF0, page, 63, t));
    CHECK_EQ(ev_count[AUTH_EV_CONSOLE_NONCE_BAD], 1);
    console_page(page, 5, 0);
    auth_console_set_report(&a, 0xF0, page, 63, t);
    console_page(page, 5, 2);  // skipped page 1
    auth_console_set_report(&a, 0xF0, page, 63, t);
    CHECK_EQ(ev_count[AUTH_EV_CONSOLE_NONCE_BAD], 2);
    CHECK_EQ(a.ds, AUTH_DS_IDLE);
    console_page(page, 5, 7);  // out of range
    auth_console_set_report(&a, 0xF0, page, 63, t);
    CHECK(auth_console_set_report(&a, 0xF0, page, 40, t));  // wrong length
    CHECK_EQ(ev_count[AUTH_EV_CONSOLE_NONCE_BAD], 4);
    CHECK(!auth_console_set_report(&a, 0x05, page, 31, t));  // not an auth report
    uint8_t buf[63];
    CHECK_EQ(auth_console_get_report(&a, 0x03, buf, 47, t), 0);  // not ours
    auth_request_t r;
    CHECK(!auth_poll(&a, t, &r));
    // F1 before a signature exists: no stale data, just nonce id and page index.
    CHECK_EQ(auth_console_get_report(&a, 0xF1, buf, 63, t), 63);
    CHECK_EQ(buf[1], 0);
    for (int i = 2; i < 63; i++) {
        CHECK_EQ(buf[i], 0);
    }
}

static void test_mismatched_completion_and_backstop(void) {
    auth_t a;
    uint32_t t = 0;
    reset_events();
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    console_send_nonce(&a, 9, t);
    auth_request_t r;
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, true, t);
    auth_complete(&a, 0xF1, 64, t + 1);  // completion for a different report: ignored
    CHECK(a.xfer_outstanding);
    CHECK_EQ(ev_count[AUTH_EV_DS_STALE_COMPLETE], 1);
    // No completion at all: backstop fires after xfer_timeout_ms.
    CHECK(!auth_poll(&a, t + a.xfer_timeout_ms, &r));
    CHECK(!auth_poll(&a, t + a.xfer_timeout_ms + 1, &r));
    CHECK_EQ(ev_count[AUTH_EV_DS_XFER_TIMEOUT], 1);
    CHECK(!a.xfer_outstanding);
    CHECK_EQ(a.ds, AUTH_DS_FAILED);
}

static void test_short_or_corrupt_downstream_replies(void) {
    auth_t a;
    hori_t h = { .busy_polls = 0 };
    uint32_t t = 0;
    reset_events();
    auth_init(&a, on_event, NULL);
    auth_set_device_present(&a, true, t);
    console_send_nonce(&a, 4, t);
    // Run until the first F1 request, then answer it short.
    auth_request_t r;
    for (int i = 0; i < 100; i++) {
        CHECK(auth_poll(&a, t, &r));
        auth_submitted(&a, true, t);
        if (r.report_id == 0xF1) {
            break;
        }
        auth_complete(&a, r.report_id, hori_execute(&h, &r), t);
        t++;
    }
    auth_complete(&a, 0xF1, 10, t);
    CHECK_EQ(ev_count[AUTH_EV_DS_STEP_FAIL], 1);
    CHECK_EQ(a.ds, AUTH_DS_SIG);
    CHECK_EQ(a.ds_page, 0);
    // A page with a bad CRC is still forwarded verbatim but reported.
    t += 10;
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, true, t);
    hori_execute(&h, &r);
    r.buf[10] ^= 0xFF;
    auth_complete(&a, 0xF1, 64, t);
    CHECK_EQ(a.ds_page, 1);
    CHECK_EQ(ev_count[AUTH_EV_DS_PAGE_CHECK], 1);  // page 0 is always logged
    // Later pages: a CRC-only difference is forwarded silently, a wrong page field is logged.
    t += 10;
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, true, t);
    hori_execute(&h, &r);
    r.buf[20] ^= 0xFF;
    auth_complete(&a, 0xF1, 64, t);
    CHECK_EQ(ev_count[AUTH_EV_DS_PAGE_CHECK], 1);
    t += 10;
    CHECK(auth_poll(&a, t, &r));
    auth_submitted(&a, true, t);
    hori_execute(&h, &r);
    r.buf[2] = 9;  // page field
    auth_complete(&a, 0xF1, 64, t);
    CHECK_EQ(ev_count[AUTH_EV_DS_PAGE_CHECK], 2);
}

int main(void) {
    RUN(test_happy_path_verbatim);
    RUN(test_waits_for_device_and_survives_loss);
    RUN(test_rejected_submission_retries);
    RUN(test_step_failures_then_give_up);
    RUN(test_sign_timeout);
    RUN(test_new_nonce_cancels_and_stale_completion_ignored);
    RUN(test_bad_console_pages);
    RUN(test_mismatched_completion_and_backstop);
    RUN(test_short_or_corrupt_downstream_replies);
    TEST_MAIN_END();
}
