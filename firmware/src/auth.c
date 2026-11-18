// PS4/PS5 controller-authentication bridge. See auth.h.
#include "auth.h"

#include <string.h>

#include "crc32.h"
#include "g29.h"

static void emit(auth_t* a, auth_event_t ev, uint8_t arg, uint32_t b) {
    if (a->ev != NULL) {
        a->ev(a->ev_ctx, ev, arg, b);
    }
}

static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

// CRC32 over the report ID followed by the first 59 payload bytes, stored LE in
// payload bytes 59..62. Used only to check and log; pages are always forwarded as-is.
static bool page_crc_ok(uint8_t report_id, const uint8_t* page) {
    uint32_t crc = crc32_update(0, &report_id, 1);
    crc = crc32_update(crc, page, AUTH_PAGE_LEN - 4);
    return crc == rd32le(&page[AUTH_PAGE_LEN - 4]);
}

const char* auth_state_name(auth_ds_state_t s) {
    switch (s) {
        case AUTH_DS_IDLE: return "idle";
        case AUTH_DS_WAIT_DEVICE: return "wait_device";
        case AUTH_DS_RESET: return "reset";
        case AUTH_DS_NONCE: return "nonce";
        case AUTH_DS_POLL: return "poll";
        case AUTH_DS_SIG: return "sig";
        case AUTH_DS_READY: return "ready";
        case AUTH_DS_FAILED: return "failed";
    }
    return "?";
}

void auth_init(auth_t* a, auth_event_cb_t ev, void* ev_ctx) {
    memset(a, 0, sizeof(*a));
    a->ev = ev;
    a->ev_ctx = ev_ctx;
    a->xfer_timeout_ms = 7000;   // TinyUSB's own control timeout is 5000 ms
    a->sign_timeout_ms = 15000;
    a->retry_delay_ms = 5;
    a->max_retries = 3;
    a->last_f2_status = 0xFF;
}

static bool job_active(const auth_t* a) {
    return a->ds != AUTH_DS_IDLE && a->ds != AUTH_DS_READY && a->ds != AUTH_DS_FAILED;
}

static void start_downstream(auth_t* a, uint32_t now) {
    a->ds = a->device_present ? AUTH_DS_RESET : AUTH_DS_WAIT_DEVICE;
    a->ds_page = 0;
    a->retries = 0;
    a->polls = 0;
    a->job_started_ms = now;
    a->next_submit_ms = now;
}

static void fail_job(auth_t* a, uint8_t report_id, uint32_t reason) {
    a->ds = AUTH_DS_FAILED;
    a->jobs_failed++;
    emit(a, AUTH_EV_DS_FAILED, report_id, reason);
}

void auth_set_device_present(auth_t* a, bool present, uint32_t now_ms) {
    if (a->device_present == present) {
        return;
    }
    a->device_present = present;
    emit(a, AUTH_EV_DEVICE_PRESENT, present, 0);
    if (!present) {
        // The host stack drops the device's transfers; nothing will complete for us.
        a->xfer_outstanding = false;
        if (job_active(a)) {
            a->ds = AUTH_DS_WAIT_DEVICE;
        }
    } else if (a->ds == AUTH_DS_WAIT_DEVICE) {
        start_downstream(a, now_ms);
    }
}

bool auth_console_set_report(auth_t* a, uint8_t report_id, const uint8_t* buf, uint16_t len, uint32_t now_ms) {
    if (report_id != 0xF0) {
        return false;
    }
    if (len != AUTH_PAGE_LEN) {
        a->collecting = false;
        emit(a, AUTH_EV_CONSOLE_NONCE_BAD, AUTH_BAD_LEN, (uint32_t) len << 16);
        return true;
    }
    uint8_t nid = buf[0];
    uint8_t page = buf[1];
    uint32_t detail = page | ((uint32_t) nid << 8) | ((uint32_t) len << 16);
    if (page >= AUTH_NONCE_PAGES) {
        a->collecting = false;
        emit(a, AUTH_EV_CONSOLE_NONCE_BAD, AUTH_BAD_PAGE_RANGE, detail);
        return true;
    }
    bool crc_ok = page_crc_ok(0xF0, buf);
    if (page == 0) {
        if (job_active(a)) {
            emit(a, AUTH_EV_JOB_CANCELLED, (uint8_t) a->ds, a->job);
            a->ds = AUTH_DS_IDLE;  // an outstanding transfer finishes as stale
        }
        a->collecting = true;
        a->nonce_id = nid;
        a->pages_rx = 0;
        a->sig_ready = false;
        a->serve_page = 0;
        a->f2_queries = 0;
        a->last_f2_status = 0xFF;
        if (a->ds == AUTH_DS_READY || a->ds == AUTH_DS_FAILED) {
            a->ds = AUTH_DS_IDLE;
        }
    } else if (!a->collecting || page != a->pages_rx || nid != a->nonce_id) {
        a->collecting = false;
        emit(a, AUTH_EV_CONSOLE_NONCE_BAD, AUTH_BAD_SEQUENCE, detail);
        return true;
    }
    memcpy(a->nonce_pages[page], buf, AUTH_PAGE_LEN);
    a->pages_rx = (uint8_t) (page + 1);
    emit(a, AUTH_EV_CONSOLE_NONCE_PAGE, page, nid | ((uint32_t) crc_ok << 8) | ((uint32_t) len << 16));
    if (a->pages_rx == AUTH_NONCE_PAGES) {
        a->collecting = false;
        a->job++;
        emit(a, AUTH_EV_JOB_START, a->nonce_id, a->job);
        start_downstream(a, now_ms);
    }
    return true;
}

uint16_t auth_console_get_report(auth_t* a, uint8_t report_id, uint8_t* buf, uint16_t reqlen, uint32_t now_ms) {
    (void) now_ms;
    switch (report_id) {
        case 0xF3: {
            memset(buf, 0, reqlen);
            memcpy(buf, g29_feature_0xf3, reqlen < sizeof(g29_feature_0xf3) ? reqlen : sizeof(g29_feature_0xf3));
            a->sig_ready = false;
            a->serve_page = 0;
            emit(a, AUTH_EV_CONSOLE_F3, 0, 0);
            return reqlen;
        }
        case 0xF2: {
            uint8_t status = a->sig_ready ? AUTH_STATUS_READY : AUTH_STATUS_BUSY;
            uint8_t tmp[15];
            memset(tmp, 0, sizeof(tmp));
            tmp[0] = a->nonce_id;
            tmp[1] = status;
            uint8_t id = 0xF2;
            uint32_t crc = crc32_update(crc32_update(0, &id, 1), tmp, 11);
            wr32le(&tmp[11], crc);
            memset(buf, 0, reqlen);
            memcpy(buf, tmp, reqlen < sizeof(tmp) ? reqlen : sizeof(tmp));
            a->f2_queries++;
            if (status != a->last_f2_status) {
                a->last_f2_status = status;
                emit(a, AUTH_EV_CONSOLE_F2, status, a->f2_queries);
            }
            return reqlen;
        }
        case 0xF1: {
            memset(buf, 0, reqlen);
            uint8_t page = a->serve_page;
            if (a->sig_ready) {
                memcpy(buf, a->sig_pages[page], reqlen < AUTH_PAGE_LEN ? reqlen : AUTH_PAGE_LEN);
            } else if (reqlen >= 2) {
                buf[0] = a->nonce_id;
                buf[1] = page;
            }
            if (page == 0 || page == AUTH_SIG_PAGES - 1 || !a->sig_ready) {
                emit(a, AUTH_EV_CONSOLE_F1_PAGE, page, a->sig_ready);
            }
            a->serve_page++;
            if (a->serve_page >= AUTH_SIG_PAGES) {
                a->serve_page = 0;
                if (a->sig_ready) {
                    a->delivered++;
                    emit(a, AUTH_EV_DELIVERED, a->nonce_id, a->delivered);
                }
            }
            return reqlen;
        }
        default:
            return 0;
    }
}

bool auth_poll(auth_t* a, uint32_t now_ms, auth_request_t* req) {
    if (a->xfer_outstanding) {
        if ((uint32_t) (now_ms - a->xfer_started_ms) > a->xfer_timeout_ms) {
            emit(a, AUTH_EV_DS_XFER_TIMEOUT, a->xfer_report_id, now_ms - a->xfer_started_ms);
            a->xfer_outstanding = false;
            if (a->xfer_job == a->job && job_active(a)) {
                fail_job(a, a->xfer_report_id, 2);
            }
        }
        return false;
    }
    if (!a->device_present || !job_active(a) || a->ds == AUTH_DS_WAIT_DEVICE) {
        return false;
    }
    if ((int32_t) (now_ms - a->next_submit_ms) < 0) {
        return false;
    }
    if (a->ds == AUTH_DS_POLL && (uint32_t) (now_ms - a->sign_started_ms) > a->sign_timeout_ms) {
        fail_job(a, 0xF2, 1);
        return false;
    }
    memset(a->xbuf, 0, sizeof(a->xbuf));
    switch (a->ds) {
        case AUTH_DS_RESET:
            req->get = true;
            req->report_id = 0xF3;
            req->len = 8;
            break;
        case AUTH_DS_NONCE:
            req->get = false;
            req->report_id = 0xF0;
            req->len = 64;
            a->xbuf[0] = 0xF0;
            memcpy(&a->xbuf[1], a->nonce_pages[a->ds_page], AUTH_PAGE_LEN);
            break;
        case AUTH_DS_POLL:
            req->get = true;
            req->report_id = 0xF2;
            req->len = 16;
            break;
        case AUTH_DS_SIG:
            req->get = true;
            req->report_id = 0xF1;
            req->len = 64;
            break;
        default:
            return false;
    }
    req->buf = a->xbuf;
    a->xfer_report_id = req->report_id;
    return true;
}

void auth_submitted(auth_t* a, bool accepted, uint32_t now_ms) {
    if (accepted) {
        a->xfer_outstanding = true;
        a->xfer_job = a->job;
        a->xfer_started_ms = now_ms;
    } else {
        a->rejected++;
        a->next_submit_ms = now_ms + a->retry_delay_ms;
        if ((a->rejected & 0x3F) == 1) {  // first, then every 64th
            emit(a, AUTH_EV_DS_SUBMIT_REJECTED, a->xfer_report_id, a->rejected);
        }
    }
}

static void step_failed(auth_t* a, uint8_t report_id, uint16_t len, uint32_t now_ms) {
    a->retries++;
    emit(a, AUTH_EV_DS_STEP_FAIL, report_id, a->retries | ((uint32_t) len << 8));
    if (a->retries > a->max_retries) {
        fail_job(a, report_id, 0);
    } else {
        a->next_submit_ms = now_ms + a->retry_delay_ms;
    }
}

void auth_complete(auth_t* a, uint8_t report_id, uint16_t len, uint32_t now_ms) {
    if (!a->xfer_outstanding || report_id != a->xfer_report_id) {
        emit(a, AUTH_EV_DS_STALE_COMPLETE, report_id, 0);
        return;
    }
    a->xfer_outstanding = false;
    if (a->xfer_job != a->job || !job_active(a)) {
        emit(a, AUTH_EV_DS_STALE_COMPLETE, report_id, 1);
        return;
    }
    switch (a->ds) {
        case AUTH_DS_RESET:
            if (len == 0) {
                step_failed(a, report_id, len, now_ms);
                return;
            }
            a->retries = 0;
            a->ds = AUTH_DS_NONCE;
            a->ds_page = 0;
            emit(a, AUTH_EV_DS_STEP_OK, report_id, 0);
            return;

        case AUTH_DS_NONCE:
            if (len != 64) {
                step_failed(a, report_id, len, now_ms);
                return;
            }
            a->retries = 0;
            emit(a, AUTH_EV_DS_STEP_OK, report_id, a->ds_page);
            a->ds_page++;
            if (a->ds_page >= AUTH_NONCE_PAGES) {
                a->ds = AUTH_DS_POLL;
                a->ds_page = 0;
                a->polls = 0;
                a->sign_started_ms = now_ms;
            }
            return;

        case AUTH_DS_POLL:
            if (len < 3 || a->xbuf[0] != 0xF2) {
                step_failed(a, report_id, len, now_ms);
                return;
            }
            a->retries = 0;
            a->polls++;
            if (a->xbuf[2] == AUTH_STATUS_READY) {
                emit(a, AUTH_EV_DS_SIGNED, (uint8_t) (a->polls > 255 ? 255 : a->polls), now_ms - a->sign_started_ms);
                a->ds = AUTH_DS_SIG;
                a->ds_page = 0;
            }
            return;

        case AUTH_DS_SIG: {
            if (len != 64 || a->xbuf[0] != 0xF1) {
                step_failed(a, report_id, len, now_ms);
                return;
            }
            a->retries = 0;
            const uint8_t* page = &a->xbuf[1];
            bool crc_ok = page_crc_ok(0xF1, page);
            bool id_ok = page[0] == a->nonce_id;
            bool page_ok = page[1] == a->ds_page;
            // Logged for page 0 (shows the CRC result) and for any id/page mismatch. The Hori
            // 0F0D:0087 does not end its F1 pages with the console-style CRC32 (seen on 131 PS5
            // rounds that the console accepted), so a CRC mismatch alone is not logged per page.
            (void) crc_ok;
            if (!id_ok || !page_ok || a->ds_page == 0) {
                emit(a, AUTH_EV_DS_PAGE_CHECK, a->ds_page,
                    (uint32_t) crc_ok | ((uint32_t) id_ok << 1) | ((uint32_t) page_ok << 2) | ((uint32_t) page[1] << 8));
            }
            memcpy(a->sig_pages[a->ds_page], page, AUTH_PAGE_LEN);
            a->ds_page++;
            if (a->ds_page >= AUTH_SIG_PAGES) {
                a->ds = AUTH_DS_READY;
                a->sig_ready = true;
                a->serve_page = 0;
                a->jobs_ok++;
                emit(a, AUTH_EV_DS_READY, 0, now_ms - a->job_started_ms);
            }
            return;
        }

        default:
            emit(a, AUTH_EV_DS_STALE_COMPLETE, report_id, 2);
            return;
    }
}
