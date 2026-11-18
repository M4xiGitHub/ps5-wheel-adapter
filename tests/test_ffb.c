// Force-feedback translation: cap enforcement, packet formats, safety stops.
#include "ffb.h"
#include "test_util.h"

// Synthetic wheelbase interface: input report 1 plus vendor output report 2 with
// 63 one-byte values (shape only; the real descriptor is captured by the adapter log).
static const uint8_t WB_DESC[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,
    0x85, 0x01, 0x09, 0x30, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x85, 0x02, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x02,
    0xC0,
};
static const uint8_t WB_SHORT_OUT[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x08, 0x91, 0x02, 0xC0,
};
static const uint8_t WB_NO_OUT[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x09, 0x30, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0,
};

static int events[16];
static void on_ev(void* ctx, ffb_event_t ev, uint8_t a, uint32_t b) {
    (void) ctx; (void) a; (void) b;
    if ((int) ev < 16) events[ev]++;
}

typedef struct {
    uint8_t p[32][FFB_OUT_MAX];
    int n;
} pkts_t;

// Sends every pending packet (all succeed) and records them.
static void drain(ffb_t* f, uint32_t now, pkts_t* out) {
    out->n = 0;
    uint8_t buf[FFB_OUT_MAX];
    for (int i = 0; i < 32; i++) {
        uint8_t n = ffb_next_packet(f, now, buf);
        if (n == 0) break;
        CHECK_EQ(n, f->out_len);
        memcpy(out->p[out->n++], buf, n);
        ffb_packet_done(f, true);
        // Only Simagic packet types ever reach the wheelbase.
        CHECK(buf[0] == SM_SET_EFFECT || buf[0] == SM_SET_CONSTANT || buf[0] == SM_EFFECT_OP);
    }
}

static int16_t mag_of(const uint8_t* p) {
    return (int16_t) (p[2] | (p[3] << 8));
}

static void console(ffb_t* f, uint32_t now, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5) {
    uint8_t r[32] = { 0x05, b0, b1, b2, b3, b4, b5, 0x00 };  // interrupt OUT shape
    ffb_console_output(f, 0, r, sizeof(r), now);
}

static void setup(ffb_t* f, uint8_t pct, bool invert) {
    memset(events, 0, sizeof(events));
    ffb_init(f, pct, invert, on_ev, NULL);
    CHECK(ffb_set_output_format(f, WB_DESC, sizeof(WB_DESC)));
    ffb_reset(f, FFB_RESET_BOOT);
}

static void test_output_format(void) {
    ffb_t f;
    ffb_init(&f, 30, false, NULL, NULL);
    CHECK(ffb_set_output_format(&f, WB_DESC, sizeof(WB_DESC)));
    CHECK_EQ(f.out_report_id, 2);
    CHECK_EQ(f.out_len, 63);
    CHECK(!ffb_set_output_format(&f, WB_SHORT_OUT, sizeof(WB_SHORT_OUT)));  // cannot hold set-effect
    CHECK(!ffb_set_output_format(&f, WB_NO_OUT, sizeof(WB_NO_OUT)));
    uint8_t buf[FFB_OUT_MAX];
    ffb_reset(&f, FFB_RESET_BOOT);
    console(&f, 0, 0x11, 0x08, 0xFF, 0x80, 0, 0);
    CHECK_EQ(ffb_next_packet(&f, 0, buf), 0);  // nothing is ever sent without a valid format
}

static void test_boot_stops_everything_first(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, false);
    console(&f, 0, 0x11, 0x08, 0xFF, 0x80, 0, 0);  // force requested before stops went out
    drain(&f, 0, &p);
    static const uint8_t blocks[6] = { 0x01, 0x02, 0x05, 0x06, 0x07, 0x09 };
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(p.p[i][0], SM_EFFECT_OP);
        CHECK_EQ(p.p[i][1], blocks[i]);
        CHECK_EQ(p.p[i][2], SM_OP_STOP);
        for (int k = 3; k < 63; k++) CHECK_EQ(p.p[i][k], 0);
    }
    CHECK_EQ(p.p[6][0], SM_SET_EFFECT);  // only then the effect
}

static void test_constant_force_sequence_and_cap(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, false);
    drain(&f, 0, &p);  // boot stops
    console(&f, 10, 0x11, 0x08, 0xFF, 0x80, 0, 0);  // lg4ff-style: slot 1 download+play, full positive
    drain(&f, 10, &p);
    CHECK_EQ(p.n, 3);
    static const uint8_t set_effect[13] = { 0x01, 0x01, 0x01, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0x04, 0x3F };
    CHECK(memcmp(p.p[0], set_effect, 13) == 0);
    CHECK_EQ(p.p[1][0], SM_SET_CONSTANT);
    CHECK_EQ(p.p[1][1], SM_BLOCK_CONSTANT);
    CHECK_EQ(mag_of(p.p[1]), 3000);  // 100 % of the console range = 30 % of the wheelbase range
    CHECK_EQ(p.p[2][0], SM_EFFECT_OP);
    CHECK_EQ(p.p[2][2], SM_OP_START);
    CHECK_EQ(events[FFB_EV_ACTIVE], 1);

    console(&f, 20, 0x1C, 0x08, 0x00, 0x80, 0, 0);  // refresh: full negative
    drain(&f, 20, &p);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(mag_of(p.p[0]), -3000);
    console(&f, 30, 0x1C, 0x08, 0x7F, 0x80, 0, 0);  // 127 = no force
    drain(&f, 30, &p);
    CHECK_EQ(mag_of(p.p[0]), 0);
    console(&f, 40, 0x1C, 0x08, 0x80, 0x80, 0, 0);  // 128 = no force, unchanged -> nothing sent
    drain(&f, 40, &p);
    CHECK_EQ(p.n, 0);
    console(&f, 50, 0x13, 0, 0, 0, 0, 0);  // stop slot 1
    drain(&f, 50, &p);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(p.p[0][0], SM_EFFECT_OP);
    CHECK_EQ(p.p[0][2], SM_OP_STOP);
    // Play again: magnitude re-sent only if it changed, then start.
    console(&f, 60, 0x11, 0x00, 0xC0, 0x80, 0x80, 0x80);  // constant type, slot F1 level 0xC0 (+64)
    drain(&f, 60, &p);
    CHECK_EQ(p.n, 2);
    CHECK_EQ(mag_of(p.p[0]), 64 * 10000 * 30 / 12700);
    CHECK_EQ(p.p[1][2], SM_OP_START);
}

static void test_cap_never_exceeded(void) {
    for (int pct = 0; pct <= 100; pct += 10) {
        ffb_t f;
        pkts_t p;
        setup(&f, (uint8_t) pct, pct % 20 == 0);
        drain(&f, 0, &p);
        uint32_t seed = 12345u + (uint32_t) pct;
        int32_t worst = 0;
        for (int i = 0; i < 5000; i++) {
            seed = seed * 1103515245u + 12345u;
            uint8_t ops[] = { 0x11, 0x31, 0xF1, 0x1C, 0x10, 0x12, 0x13, 0xF3, 0xF8, 0xF5, 0x51 };
            uint8_t b0 = ops[(seed >> 8) % sizeof(ops)];
            uint8_t types[] = { 0x00, 0x08, 0x01, 0x0B };
            console(&f, (uint32_t) i, b0, types[(seed >> 4) & 3], (uint8_t) (seed >> 16), (uint8_t) (seed >> 24),
                (uint8_t) seed, (uint8_t) (seed >> 3));
            drain(&f, (uint32_t) i, &p);
            for (int k = 0; k < p.n; k++) {
                if (p.p[k][0] == SM_SET_CONSTANT) {
                    int32_t m = mag_of(p.p[k]);
                    if (m < 0) m = -m;
                    if (m > worst) worst = m;
                }
            }
        }
        CHECK(worst <= pct * 100);
    }
}

static void test_multi_slot_sum_and_invert(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 100, false);
    drain(&f, 0, &p);
    console(&f, 1, 0x31, 0x00, 0xFF, 0xFF, 0x80, 0x80);  // F1 and F2 both full positive: clamped
    drain(&f, 1, &p);
    CHECK_EQ(mag_of(p.p[1]), 10000);
    setup(&f, 30, true);
    drain(&f, 0, &p);
    console(&f, 1, 0x11, 0x08, 0xFF, 0x80, 0, 0);
    drain(&f, 1, &p);
    CHECK_EQ(mag_of(p.p[1]), -3000);  // inverted
}

static void test_ignored_commands(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, false);
    drain(&f, 0, &p);
    console(&f, 1, 0xF8, 0x12, 0x1F, 0, 0, 0);  // RPM LEDs
    console(&f, 2, 0xF8, 0x81, 0x84, 0x03, 0, 0);  // 900 degree range
    console(&f, 3, 0xF8, 0x09, 0x05, 0x01, 0, 0);  // mode switch: never forwarded
    console(&f, 4, 0xF5, 0, 0, 0, 0, 0);  // autocenter off
    console(&f, 5, 0xFE, 0x0D, 0x02, 0x02, 0x10, 0);  // autocenter parameters
    drain(&f, 5, &p);
    CHECK_EQ(p.n, 0);
    CHECK_EQ(f.ext_cmds, 5);  // three 0xF8 extended, 0xF5 (spring off), 0xFE (spring parameters)
    console(&f, 6, 0x11, 0x01, 0x20, 0x20, 0x33, 0x00);  // spring: not translated
    drain(&f, 6, &p);
    CHECK_EQ(p.n, 0);
    CHECK_EQ(f.unsupported_by_type[1], 1);
    CHECK_EQ(events[FFB_EV_UNSUPPORTED], 1);
}

static void test_control_transfer_shape(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, false);
    drain(&f, 0, &p);
    uint8_t r[31] = { 0x11, 0x08, 0xFF, 0x80, 0, 0, 0 };  // SET_REPORT(output 5), ID stripped
    ffb_console_output(&f, 5, r, sizeof(r), 1);
    drain(&f, 1, &p);
    CHECK_EQ(p.n, 3);
    CHECK_EQ(mag_of(p.p[1]), 3000);
    ffb_console_output(&f, 0, r, 4, 2);  // too short
    ffb_console_output(&f, 3, r, 31, 2);  // other report id
    CHECK_EQ(f.rx_bad, 2);
}

static void test_held_force_and_safety_stops(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, false);
    drain(&f, 0, &p);
    console(&f, 100, 0x11, 0x08, 0xFF, 0x80, 0, 0);
    drain(&f, 100, &p);
    CHECK(f.running);
    // GT7 sends only on change: a force is held however long the console is quiet.
    drain(&f, 100 + 2001, &p);
    CHECK_EQ(p.n, 0);
    drain(&f, 100 + 600000, &p);
    CHECK_EQ(p.n, 0);
    CHECK(f.running);
    CHECK_EQ(f.sent_mag, 3000);
    // A refresh after a long quiet period updates the playing force directly.
    console(&f, 700000, 0x1C, 0x08, 0x40, 0x80, 0, 0);
    drain(&f, 700000, &p);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(mag_of(p.p[0]), -(127 - 0x40) * 10000 * 30 / 12700);
    // Console unconfigured / suspended: stop, and nothing restarts by itself.
    ffb_reset(&f, FFB_RESET_CONSOLE_GONE);
    drain(&f, 700001, &p);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(p.p[0][2], SM_OP_STOP);
    drain(&f, 800000, &p);
    CHECK_EQ(p.n, 0);
    CHECK(!f.running);
    CHECK_EQ(events[FFB_EV_STALE], 0);
}

static void test_failed_send_is_retried(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, false);
    drain(&f, 0, &p);
    console(&f, 1, 0x11, 0x08, 0xFF, 0x80, 0, 0);
    uint8_t a[FFB_OUT_MAX], b[FFB_OUT_MAX];
    CHECK(ffb_next_packet(&f, 1, a) > 0);
    CHECK_EQ(ffb_next_packet(&f, 1, b), 0);  // one packet in flight at a time
    ffb_packet_done(&f, false);
    CHECK(ffb_next_packet(&f, 2, b) > 0);
    CHECK(memcmp(a, b, f.out_len) == 0);
    CHECK_EQ(f.tx_failed, 1);
}

static void test_bench_test_mode(void) {
    ffb_t f;
    pkts_t p;
    setup(&f, 30, true);  // invert does not apply to the raw bench test
    drain(&f, 0, &p);
    ffb_test(&f, 1000, 500, 10);
    drain(&f, 10, &p);
    CHECK_EQ(p.n, 3);
    CHECK_EQ(mag_of(p.p[1]), 300);  // capped: 30 % of 1000
    drain(&f, 509, &p);
    CHECK_EQ(p.n, 0);
    drain(&f, 511, &p);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(p.p[0][2], SM_OP_STOP);
}

int main(void) {
    RUN(test_output_format);
    RUN(test_boot_stops_everything_first);
    RUN(test_constant_force_sequence_and_cap);
    RUN(test_cap_never_exceeded);
    RUN(test_multi_slot_sum_and_invert);
    RUN(test_ignored_commands);
    RUN(test_control_transfer_shape);
    RUN(test_held_force_and_safety_stops);
    RUN(test_failed_send_is_retried);
    RUN(test_bench_test_mode);
    TEST_MAIN_END();
}
