// Input mapping (Simagic/VRS -> G29) and downstream device routing decisions.
#include "devices.h"
#include "fixtures.h"
#include "input_map.h"
#include "test_util.h"

static const button_map_entry_t MAP[] = {
    { 6, G29_BTN_PS },
    { 13, G29_BTN_R1 },
    { 1, G29_BTN_CROSS },
    { 20, G29_BTN_DPAD_UP },
    { 21, G29_BTN_DPAD_RIGHT },
};
static const input_config_t CFG = { MAP, sizeof(MAP) / sizeof(MAP[0]), false, true };

static void setup(input_state_t* s) {
    memset(s, 0, sizeof(*s));
    uint8_t mm;
    CHECK_EQ(wheel_layout_from_desc(SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), &s->wheel_layout, &mm), LAYOUT_MATCHES_DOC);
    CHECK_EQ(mm, 0);
    s->wheel_layout_ok = true;
    CHECK_EQ(pedal_layout_from_desc(VRS_DOC_DESC, sizeof(VRS_DOC_DESC), &s->pedal_layout, &mm), LAYOUT_MATCHES_DOC);
    CHECK_EQ(mm, 0);
    s->pedal_layout_ok = true;
}

// 39-byte Simagic packet: report ID 1, steering at bytes 1-2, hat low nibble of byte 12,
// button n at packet bit 100 + n - 1.
static void simagic_packet(uint8_t* p, uint16_t steer, uint8_t hat, const int* buttons, int nb) {
    memset(p, 0, 39);
    p[0] = 1;
    p[1] = (uint8_t) steer;
    p[2] = (uint8_t) (steer >> 8);
    p[12] = (uint8_t) (hat & 0x0F);
    for (int i = 0; i < nb; i++) {
        int bit = 100 + buttons[i] - 1;
        p[bit / 8] |= (uint8_t) (1u << (bit % 8));
    }
}

static void vrs_packet(uint8_t* p, uint16_t x, uint16_t y, uint16_t z) {
    memset(p, 0, 64);
    p[0] = 1;
    p[1] = (uint8_t) x; p[2] = (uint8_t) (x >> 8);
    p[3] = (uint8_t) y; p[4] = (uint8_t) (y >> 8);
    p[5] = (uint8_t) z; p[6] = (uint8_t) (z >> 8);
}

static void test_scale(void) {
    for (int32_t v = 0; v <= 65535; v += 4369) {
        CHECK_EQ(input_scale_u16(v, 0, 65535), v);  // 16-bit sources keep every value
    }
    CHECK_EQ(input_scale_u16(0, 0, 4095), 0);
    CHECK_EQ(input_scale_u16(4095, 0, 4095), 65535);
    CHECK_EQ(input_scale_u16(-5, 0, 4095), 0);       // clamped
    CHECK_EQ(input_scale_u16(5000, 0, 4095), 65535);
    CHECK_EQ(input_scale_u16(-32768, -32768, 32767), 0);
    CHECK_EQ(input_scale_u16(32767, -32768, 32767), 65535);
    CHECK_EQ(input_scale_u16(0, -32768, 32767), 32768);
    // strictly monotonic for a 12-bit source: no two inputs collapse
    uint16_t prev = 0;
    for (int32_t v = 1; v <= 4095; v++) {
        uint16_t o = input_scale_u16(v, 0, 4095);
        CHECK(o > prev);
        prev = o;
    }
}

static void test_neutral_without_sources(void) {
    input_state_t s;
    setup(&s);
    g29_report_t r;
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), 0x8000);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_CLUTCH), 0xFFFF);
    CHECK_EQ(g29_get_dpad(&r), G29_DPAD_NEUTRAL);
    CHECK(!g29_get_button(&r, G29_BTN_PS));
}

static void test_steering_full_resolution(void) {
    input_state_t s;
    setup(&s);
    uint8_t p[39];
    g29_report_t r;
    const uint16_t vals[] = { 0, 1, 32767, 32768, 40000, 65534, 65535 };
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        simagic_packet(p, vals[i], 8, NULL, 0);
        CHECK(input_wheel_report(&s, p, 39));
        input_build_report(&s, &CFG, &r);
        CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), vals[i]);
    }
    input_config_t inv = CFG;
    inv.steer_invert = true;
    simagic_packet(p, 1000, 8, NULL, 0);
    CHECK(input_wheel_report(&s, p, 39));
    input_build_report(&s, &inv, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), 65535 - 1000);
}

static void test_hat_and_dpad_buttons(void) {
    input_state_t s;
    setup(&s);
    uint8_t p[39];
    g29_report_t r;
    for (uint8_t h = 0; h < 8; h++) {
        simagic_packet(p, 32768, h, NULL, 0);
        input_wheel_report(&s, p, 39);
        input_build_report(&s, &CFG, &r);
        CHECK_EQ(g29_get_dpad(&r), h);
    }
    simagic_packet(p, 32768, 8, NULL, 0);  // null state
    input_wheel_report(&s, p, 39);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_dpad(&r), G29_DPAD_NEUTRAL);
    simagic_packet(p, 32768, 15, NULL, 0);
    input_wheel_report(&s, p, 39);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_dpad(&r), G29_DPAD_NEUTRAL);
    const int up_right[] = { 20, 21 };
    simagic_packet(p, 32768, 8, up_right, 2);
    input_wheel_report(&s, p, 39);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_dpad(&r), 1);  // north-east from mapped buttons
}

static void test_ps_button_independent(void) {
    input_state_t s;
    setup(&s);
    uint8_t p[39], v[64];
    g29_report_t r;
    vrs_packet(v, 0, 30000, 50000);
    CHECK(input_pedal_report(&s, v, 64));
    const int b6[] = { 6 };
    simagic_packet(p, 12345, 2, b6, 1);
    CHECK(input_wheel_report(&s, p, 39));
    input_build_report(&s, &CFG, &r);
    CHECK(g29_get_button(&r, G29_BTN_PS));
    CHECK(!g29_get_button(&r, G29_BTN_R1));
    CHECK(!g29_get_button(&r, G29_BTN_CROSS));
    CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), 12345);  // driving values untouched by PS
    CHECK_EQ(g29_get_dpad(&r), 2);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 65535 - 50000);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 65535 - 30000);
    const int b13[] = { 13 };
    simagic_packet(p, 12345, 2, b13, 1);
    input_wheel_report(&s, p, 39);
    input_build_report(&s, &CFG, &r);
    CHECK(!g29_get_button(&r, G29_BTN_PS));
    CHECK(g29_get_button(&r, G29_BTN_R1));
    CHECK(input_wheel_button(&s, 13));
    CHECK(!input_wheel_button(&s, 6));
    // An unmapped source button changes nothing in the report.
    const int b50[] = { 50 };
    g29_report_t r0;
    simagic_packet(p, 12345, 2, NULL, 0);
    input_wheel_report(&s, p, 39);
    input_build_report(&s, &CFG, &r0);
    simagic_packet(p, 12345, 2, b50, 1);
    input_wheel_report(&s, p, 39);
    input_build_report(&s, &CFG, &r);
    CHECK(memcmp(&r0, &r, sizeof(r)) == 0);
}

static void test_pedals(void) {
    input_state_t s;
    setup(&s);
    uint8_t v[64];
    g29_report_t r;
    vrs_packet(v, 40000, 0, 0);  // released; X (clutch source) ignored
    CHECK(input_pedal_report(&s, v, 64));
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_CLUTCH), 0xFFFF);
    vrs_packet(v, 0, 65535, 0);  // full brake only (Y)
    input_pedal_report(&s, v, 64);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 0);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0xFFFF);
    vrs_packet(v, 0, 0, 65535);  // full throttle only (Z)
    input_pedal_report(&s, v, 64);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 0xFFFF);
    input_config_t falling = CFG;
    falling.pedals_rise_when_pressed = false;
    input_build_report(&s, &falling, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0xFFFF);
}

static void test_reject_and_loss(void) {
    input_state_t s;
    setup(&s);
    uint8_t p[39];
    g29_report_t r;
    simagic_packet(p, 1000, 8, NULL, 0);
    CHECK(input_wheel_report(&s, p, 39));
    uint8_t bad[39];
    memcpy(bad, p, 39);
    bad[0] = 2;  // different report ID
    bad[1] = 0xFF;
    CHECK(!input_wheel_report(&s, bad, 39));
    CHECK(!input_wheel_report(&s, p, 20));  // short
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), 1000);  // previous values kept
    input_wheel_lost(&s);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), 0x8000);  // neutral after removal
    CHECK(!input_wheel_report(&s, p, 39));  // no layout until re-mounted
    uint8_t v[64];
    vrs_packet(v, 0, 65535, 65535);
    CHECK(input_pedal_report(&s, v, 64));
    input_pedal_lost(&s);
    input_build_report(&s, &CFG, &r);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 0xFFFF);
}

static void test_layout_validation(void) {
    wheel_layout_t wl;
    pedal_layout_t pl;
    uint8_t mm;
    // A wheel descriptor with steering elsewhere: still usable, flagged as differing.
    CHECK_EQ(wheel_layout_from_desc(VRS_DOC_DESC, sizeof(VRS_DOC_DESC), &wl, &mm), LAYOUT_DIFFERS);
    CHECK(mm & LAYOUT_MM_AXIS2);  // no hat
    CHECK(mm & LAYOUT_MM_AXIS3);  // no buttons
    // No X axis at all: unusable.
    CHECK_EQ(wheel_layout_from_desc(PLAIN_VENDOR_DESC, sizeof(PLAIN_VENDOR_DESC), &wl, &mm), LAYOUT_UNUSABLE);
    CHECK_EQ(pedal_layout_from_desc(PLAIN_VENDOR_DESC, sizeof(PLAIN_VENDOR_DESC), &pl, &mm), LAYOUT_UNUSABLE);
    // The Simagic-shaped descriptor has Y and Z too, but X/Y/Z positions differ from VRS.
    CHECK_EQ(pedal_layout_from_desc(SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), &pl, &mm), LAYOUT_DIFFERS);
    CHECK(mm & LAYOUT_MM_REPORT);
}

static void test_routing_by_vid_pid_and_descriptor(void) {
    wheel_layout_t wl;
    pedal_layout_t pl;
    hid_decision_t d;
    // Hori with auth reports -> auth.
    d = devices_decide_hid(0x0F0D, 0x0087, AUTH_STYLE_DESC, sizeof(AUTH_STYLE_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_AUTH);
    CHECK_EQ(d.auth.bits_f1, 63 * 8);
    // Hori VID/PID without auth reports (wrong mode / other interface) -> never auth.
    d = devices_decide_hid(0x0F0D, 0x0087, PLAIN_VENDOR_DESC, sizeof(PLAIN_VENDOR_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_PASSIVE);
    // Any other device with auth-shaped reports is still not an auth target.
    d = devices_decide_hid(0x0FD9, 0x0063, AUTH_STYLE_DESC, sizeof(AUTH_STYLE_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_PASSIVE);
    d = devices_decide_hid(0x0483, 0x0522, AUTH_STYLE_DESC, sizeof(AUTH_STYLE_DESC), &wl, &pl);
    CHECK(d.role != ROLE_AUTH);
    d = devices_decide_hid(0x1234, 0x5678, AUTH_STYLE_DESC, sizeof(AUTH_STYLE_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_UNKNOWN);
    d = devices_decide_hid(0x8087, 0x1024, AUTH_STYLE_DESC, sizeof(AUTH_STYLE_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_PASSIVE);
    // Wheel / pedals need a usable layout from their own descriptor.
    d = devices_decide_hid(0x0483, 0x0522, SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_WHEEL);
    CHECK_EQ(d.layout, LAYOUT_MATCHES_DOC);
    d = devices_decide_hid(0x0483, 0x0522, PLAIN_VENDOR_DESC, sizeof(PLAIN_VENDOR_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_PASSIVE);  // e.g. a vendor interface of the composite wheel
    d = devices_decide_hid(0x0483, 0xA3BE, VRS_DOC_DESC, sizeof(VRS_DOC_DESC), &wl, &pl);
    CHECK_EQ(d.role, ROLE_PEDALS);
    CHECK_EQ(d.layout, LAYOUT_MATCHES_DOC);
    // Same STMicro VID, different PIDs, different roles.
    CHECK_EQ(devices_lookup(0x0483, 0x0522)->role, ROLE_WHEEL);
    CHECK_EQ(devices_lookup(0x0483, 0xA3BE)->role, ROLE_PEDALS);
    CHECK_EQ(devices_lookup(0x05E3, 0x0610)->role, ROLE_HUB);
    CHECK(devices_lookup(0x0483, 0x5740) == NULL);
}

static void test_port_expectations(void) {
    const known_device_t* hori = devices_lookup(0x0F0D, 0x0087);
    const known_device_t* wheel = devices_lookup(0x0483, 0x0522);
    const known_device_t* hub = devices_lookup(0x05E3, 0x0610);
    CHECK(devices_port_ok(hori, PORT_A_RHPORT, 0));
    CHECK(!devices_port_ok(hori, PORT_B_RHPORT, 0));
    CHECK(!devices_port_ok(hori, PORT_A_RHPORT, 3));
    CHECK(devices_port_ok(wheel, PORT_B_RHPORT, 2));
    CHECK(!devices_port_ok(wheel, PORT_B_RHPORT, 0));
    CHECK(devices_port_ok(hub, PORT_B_RHPORT, 0));
    CHECK(!devices_port_ok(NULL, PORT_A_RHPORT, 0));
}

int main(void) {
    RUN(test_scale);
    RUN(test_neutral_without_sources);
    RUN(test_steering_full_resolution);
    RUN(test_hat_and_dpad_buttons);
    RUN(test_ps_button_independent);
    RUN(test_pedals);
    RUN(test_reject_and_loss);
    RUN(test_layout_validation);
    RUN(test_routing_by_vid_pid_and_descriptor);
    RUN(test_port_expectations);
    TEST_MAIN_END();
}
