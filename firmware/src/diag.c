// Diagnostic event log. See diag.h.
#include "diag.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"

#define RECS_PER_PAGE (DIAG_FLASH_PAGE / sizeof(diag_rec_t))

void diag_init(diag_t* d) {
    memset(d, 0, sizeof(*d));
    d->flush_interval_ms = 2000;
    d->urgent_delay_ms = 50;
}

static bool header_valid(const diag_slot_header_t* h) {
    return h->magic == DIAG_SLOT_MAGIC && h->version == DIAG_SLOT_VERSION &&
           h->header_crc == crc32_calc((const uint8_t*) h, offsetof(diag_slot_header_t, header_crc));
}

bool diag_flash_open(diag_t* d, const diag_flash_ops_t* ops, uint32_t fw_version, uint8_t variant,
    uint8_t reset_reason, uint16_t clk_mhz) {
    d->ops = ops;
    d->flash_active = false;

    uint32_t newest = 0;
    int newest_slot = -1;
    for (uint32_t s = 0; s < DIAG_FLASH_SLOTS; s++) {
        diag_slot_header_t h;
        if (!ops->read(ops->ctx, DIAG_FLASH_REGION_OFFSET + s * DIAG_FLASH_SLOT_SIZE, &h, sizeof(h))) {
            return false;
        }
        if (header_valid(&h) && (newest_slot < 0 || (int32_t) (h.boot_counter - newest) > 0)) {
            newest = h.boot_counter;
            newest_slot = (int) s;
        }
    }
    d->slot = (newest_slot < 0) ? 0 : (uint8_t) ((newest_slot + 1) % DIAG_FLASH_SLOTS);
    d->boot_counter = (newest_slot < 0) ? 1 : newest + 1;
    d->slot_base = DIAG_FLASH_REGION_OFFSET + (uint32_t) d->slot * DIAG_FLASH_SLOT_SIZE;

    if (!ops->erase(ops->ctx, d->slot_base, DIAG_FLASH_SLOT_SIZE)) {
        return false;
    }
    uint8_t page[DIAG_FLASH_PAGE];
    memset(page, 0xFF, sizeof(page));
    diag_slot_header_t h = {
        .magic = DIAG_SLOT_MAGIC,
        .version = DIAG_SLOT_VERSION,
        .rec_size = sizeof(diag_rec_t),
        .boot_counter = d->boot_counter,
        .fw_version = fw_version,
        .variant = variant,
        .reset_reason = reset_reason,
        .clk_mhz = clk_mhz,
        .slot_size = DIAG_FLASH_SLOT_SIZE,
        .header_crc = 0,
    };
    h.header_crc = crc32_calc((const uint8_t*) &h, offsetof(diag_slot_header_t, header_crc));
    memcpy(page, &h, sizeof(h));
    if (!ops->program(ops->ctx, d->slot_base, page, sizeof(page))) {
        return false;
    }
    d->flash_wpos = DIAG_FLASH_PAGE;  // records start at the second page
    d->flash_tail = (d->head > DIAG_RING) ? d->head - DIAG_RING : 0;  // keep records pushed before open
    d->flash_active = true;
    return true;
}

void diag_push(diag_t* d, uint32_t now_ms, uint8_t type, uint8_t a, const uint8_t* data8, bool urgent) {
    if (d->head - d->flash_tail >= DIAG_RING) {
        d->flash_tail++;
        if (d->flash_active) {
            d->flash_dropped++;
        }
    }
    if (d->head - d->live_tail >= DIAG_RING) {
        d->live_tail++;
        if (d->live_enabled) {
            d->live_dropped++;
        }
    }
    diag_rec_t* r = &d->ring[d->head % DIAG_RING];
    r->t_ms = now_ms;
    r->seq = d->seq++;
    r->type = type;
    r->a = a;
    if (data8 != NULL) {
        memcpy(r->d, data8, sizeof(r->d));
    } else {
        memset(r->d, 0, sizeof(r->d));
    }
    d->head++;
    if (urgent && !d->urgent) {
        d->urgent = true;
        d->urgent_since_ms = now_ms;
    }
}

void diag_push_u32(diag_t* d, uint32_t now_ms, uint8_t type, uint8_t a, uint32_t v0, uint32_t v1, bool urgent) {
    uint8_t b[8];
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t) (v0 >> (8 * i));
        b[4 + i] = (uint8_t) (v1 >> (8 * i));
    }
    diag_push(d, now_ms, type, a, b, urgent);
}

bool diag_flash_service(diag_t* d, uint32_t now_ms) {
    if (!d->flash_active) {
        return false;
    }
    uint32_t pending = d->head - d->flash_tail;
    if (pending == 0) {
        d->urgent = false;
        return false;
    }
    uint32_t first = (d->flash_wpos % DIAG_FLASH_PAGE) / sizeof(diag_rec_t);
    uint32_t room_in_page = RECS_PER_PAGE - first;
    bool due = pending >= room_in_page ||
               (uint32_t) (now_ms - d->last_flush_ms) >= d->flush_interval_ms ||
               (d->urgent && (uint32_t) (now_ms - d->urgent_since_ms) >= d->urgent_delay_ms);
    if (!due) {
        return false;
    }
    uint32_t room_in_slot = (DIAG_FLASH_SLOT_SIZE - d->flash_wpos) / sizeof(diag_rec_t);
    if (room_in_slot == 0) {
        d->flash_active = false;
        return false;
    }
    uint32_t n = pending;
    if (n > room_in_page) {
        n = room_in_page;
    }
    // Keep the last record of the slot for DREC_LOG_FULL.
    bool last_page = room_in_slot <= RECS_PER_PAGE;
    if (last_page && n >= room_in_slot) {
        n = room_in_slot - 1;
    }

    uint8_t page[DIAG_FLASH_PAGE];
    memset(page, 0xFF, sizeof(page));  // 0xFF leaves already-programmed bytes unchanged
    for (uint32_t i = 0; i < n; i++) {
        memcpy(&page[(first + i) * sizeof(diag_rec_t)], &d->ring[(d->flash_tail + i) % DIAG_RING], sizeof(diag_rec_t));
    }
    if (last_page && n == room_in_slot - 1) {
        diag_rec_t full;
        memset(&full, 0, sizeof(full));
        full.t_ms = now_ms;
        full.seq = 0xFFFF;
        full.type = DREC_LOG_FULL;
        memcpy(&page[(first + n) * sizeof(diag_rec_t)], &full, sizeof(full));
    }
    uint32_t page_ofs = d->slot_base + (d->flash_wpos - (d->flash_wpos % DIAG_FLASH_PAGE));
    if (!d->ops->program(d->ops->ctx, page_ofs, page, sizeof(page))) {
        d->flash_active = false;
        return true;
    }
    d->flash_tail += n;
    d->flash_wpos += n * sizeof(diag_rec_t);
    if (last_page && n == room_in_slot - 1) {
        d->flash_wpos = DIAG_FLASH_SLOT_SIZE;
        d->flash_active = false;
    }
    d->last_flush_ms = now_ms;
    if (d->head == d->flash_tail) {
        d->urgent = false;
    }
    return true;
}

bool diag_live_peek(const diag_t* d, diag_rec_t* out) {
    if (!d->live_enabled || d->live_tail == d->head) {
        return false;
    }
    *out = d->ring[d->live_tail % DIAG_RING];
    return true;
}

void diag_live_consume(diag_t* d) {
    if (d->live_tail != d->head) {
        d->live_tail++;
    }
}
