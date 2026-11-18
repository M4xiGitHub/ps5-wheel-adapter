// Diagnostic log: per-boot flash slots, NOR-flash write semantics, bounded ring,
// non-blocking consumers.
#include <stddef.h>

#include "crc32.h"
#include "diag.h"
#include "test_util.h"

#define FLASH_SIZE (2u * 1024u * 1024u)
static uint8_t flash[FLASH_SIZE];
static int programs, erases;
static bool fail_program;

static bool f_read(void* ctx, uint32_t ofs, void* buf, uint32_t len) {
    (void) ctx;
    CHECK(ofs + len <= FLASH_SIZE);
    memcpy(buf, &flash[ofs], len);
    return true;
}

static bool f_erase(void* ctx, uint32_t ofs, uint32_t len) {
    (void) ctx;
    CHECK_EQ(ofs % 4096, 0);
    CHECK_EQ(len % 4096, 0);
    CHECK(ofs >= DIAG_FLASH_REGION_OFFSET);
    memset(&flash[ofs], 0xFF, len);
    erases++;
    return true;
}

// NOR semantics: programming can only clear bits.
static bool f_program(void* ctx, uint32_t ofs, const void* buf, uint32_t len) {
    (void) ctx;
    CHECK_EQ(ofs % 256, 0);
    CHECK_EQ(len, 256);
    CHECK(ofs >= DIAG_FLASH_REGION_OFFSET);
    if (fail_program) {
        return false;
    }
    const uint8_t* b = (const uint8_t*) buf;
    for (uint32_t i = 0; i < len; i++) {
        flash[ofs + i] &= b[i];
    }
    programs++;
    return true;
}

static const diag_flash_ops_t OPS = { f_read, f_erase, f_program, NULL };

static diag_t d;  // large; keep off the stack

static const diag_rec_t* flash_rec(uint8_t slot, uint32_t index) {
    return (const diag_rec_t*) &flash[DIAG_FLASH_REGION_OFFSET + slot * DIAG_FLASH_SLOT_SIZE + 256 + index * 16];
}

static void blank(void) {
    memset(flash, 0xFF, sizeof(flash));
    programs = erases = 0;
    fail_program = false;
}

static void test_slot_rotation(void) {
    blank();
    for (uint32_t boot = 1; boot <= 6; boot++) {
        diag_init(&d);
        CHECK(diag_flash_open(&d, &OPS, 0x000100, 0, 0, 192));
        CHECK_EQ(d.boot_counter, boot);
        CHECK_EQ(d.slot, (boot - 1) % DIAG_FLASH_SLOTS);
        diag_slot_header_t h;
        memcpy(&h, &flash[d.slot_base], sizeof(h));
        CHECK_EQ(h.magic, DIAG_SLOT_MAGIC);
        CHECK_EQ(h.boot_counter, boot);
        CHECK_EQ(h.header_crc, crc32_calc((const uint8_t*) &h, offsetof(diag_slot_header_t, header_crc)));
    }
    // A corrupted header (e.g. power lost during erase) is ignored.
    flash[DIAG_FLASH_REGION_OFFSET + 1 * DIAG_FLASH_SLOT_SIZE + 4] ^= 0x01;  // slot 1 holds boot 6
    diag_init(&d);
    CHECK(diag_flash_open(&d, &OPS, 1, 0, 0, 192));
    CHECK_EQ(d.boot_counter, 6);  // newest valid is boot 5 in slot 0
    CHECK_EQ(d.slot, 1);
}

static void test_previous_session_survives_new_boot(void) {
    blank();
    diag_init(&d);
    CHECK(diag_flash_open(&d, &OPS, 1, 0, 0, 192));  // "console session"
    for (int i = 0; i < 40; i++) {
        diag_push_u32(&d, 100 + i, DREC_AUTH, 1, (uint32_t) i, 0, false);
    }
    // Full pages go out at once; the partial tail waits for the flush interval.
    // This is the documented power-loss window (<= 2 s, 50 ms for urgent records).
    while (diag_flash_service(&d, 10000)) {
    }
    CHECK_EQ(flash_rec(0, 31)->d[0], 31);
    CHECK_EQ(flash_rec(0, 32)->type, 0xFF);  // still in RAM
    CHECK(diag_flash_service(&d, 12000));
    // Next boot (e.g. on the PC) uses another slot and leaves slot 0 intact.
    diag_init(&d);
    CHECK(diag_flash_open(&d, &OPS, 1, 1, 0, 192));
    CHECK_EQ(d.slot, 1);
    for (int i = 0; i < 40; i++) {
        CHECK_EQ(flash_rec(0, (uint32_t) i)->d[0], i);
        CHECK_EQ(flash_rec(0, (uint32_t) i)->seq, i);
    }
    CHECK_EQ(flash_rec(0, 40)->type, 0xFF);  // unwritten
}

static void test_flush_policy_and_partial_pages(void) {
    blank();
    diag_init(&d);
    CHECK(diag_flash_open(&d, &OPS, 1, 0, 0, 192));
    int p0 = programs;
    diag_push(&d, 1, DREC_HEARTBEAT, 0, NULL, false);
    CHECK(!diag_flash_service(&d, 10));     // not due yet
    CHECK(diag_flash_service(&d, 2001));    // interval elapsed
    CHECK_EQ(programs, p0 + 1);
    diag_push(&d, 2100, DREC_ERROR, 7, NULL, true);  // urgent
    CHECK(!diag_flash_service(&d, 2120));
    CHECK(diag_flash_service(&d, 2150));
    // Both records are in the same page; the second write did not disturb the first.
    CHECK_EQ(flash_rec(0, 0)->type, DREC_HEARTBEAT);
    CHECK_EQ(flash_rec(0, 1)->type, DREC_ERROR);
    CHECK_EQ(flash_rec(0, 1)->a, 7);
    // A full page is written immediately.
    for (int i = 0; i < 14; i++) {
        diag_push(&d, 2200, DREC_HEARTBEAT2, (uint8_t) i, NULL, false);
    }
    CHECK(diag_flash_service(&d, 2201));
    CHECK_EQ(flash_rec(0, 15)->a, 13);
    CHECK(!diag_flash_service(&d, 2202));  // nothing pending
}

static void test_ring_overflow_counts_drops(void) {
    blank();
    diag_init(&d);
    d.live_enabled = true;
    CHECK(diag_flash_open(&d, &OPS, 1, 0, 0, 192));
    for (uint32_t i = 0; i < DIAG_RING + 88; i++) {
        diag_push_u32(&d, i, DREC_HEARTBEAT, 0, i, 0, false);
    }
    CHECK_EQ(d.flash_dropped, 88);
    CHECK_EQ(d.live_dropped, 88);
    diag_rec_t r;
    CHECK(diag_live_peek(&d, &r));
    CHECK_EQ(r.d[0] | (r.d[1] << 8), 88);  // oldest kept
    // The flash sink drains independently of the live sink.
    while (diag_flash_service(&d, 100000)) {
    }
    CHECK_EQ(flash_rec(0, 0)->d[0] | (flash_rec(0, 0)->d[1] << 8), 88);
    CHECK_EQ(flash_rec(0, DIAG_RING - 1)->d[0] | (flash_rec(0, DIAG_RING - 1)->d[1] << 8), (DIAG_RING + 87) & 0xFFFF);
    CHECK(diag_live_peek(&d, &r));  // live data still there
    for (uint32_t i = 0; i < DIAG_RING; i++) {
        diag_live_consume(&d);
    }
    CHECK(!diag_live_peek(&d, &r));
}

static void test_slot_full_and_flash_error(void) {
    blank();
    diag_init(&d);
    CHECK(diag_flash_open(&d, &OPS, 1, 0, 0, 192));
    uint32_t capacity = (DIAG_FLASH_SLOT_SIZE - 256) / 16;
    uint32_t t = 0;
    for (uint32_t i = 0; i < capacity + 100; i++) {
        diag_push(&d, t, DREC_HEARTBEAT, 0, NULL, false);
        if ((i & 15) == 15) {
            diag_flash_service(&d, ++t);
        }
    }
    while (diag_flash_service(&d, t += 3000)) {
    }
    CHECK(!d.flash_active);
    CHECK_EQ(flash_rec(0, capacity - 1)->type, DREC_LOG_FULL);
    CHECK_EQ(flash_rec(0, capacity - 2)->type, DREC_HEARTBEAT);
    // Next slot untouched.
    CHECK_EQ(flash[DIAG_FLASH_REGION_OFFSET + DIAG_FLASH_SLOT_SIZE], 0xFF);

    blank();
    diag_init(&d);
    CHECK(diag_flash_open(&d, &OPS, 1, 0, 0, 192));
    fail_program = true;
    diag_push(&d, 0, DREC_HEARTBEAT, 0, NULL, true);
    diag_flash_service(&d, 100);
    CHECK(!d.flash_active);  // flash errors disable the sink, never block
    diag_push(&d, 0, DREC_HEARTBEAT, 0, NULL, true);
    CHECK(!diag_flash_service(&d, 200));
}

int main(void) {
    RUN(test_slot_rotation);
    RUN(test_previous_session_survives_new_boot);
    RUN(test_flush_policy_and_partial_pages);
    RUN(test_ring_overflow_counts_drops);
    RUN(test_slot_full_and_flash_error);
    TEST_MAIN_END();
}
