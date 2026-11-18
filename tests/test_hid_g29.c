// HID descriptor parser and console-facing G29 report sizes/offsets.
#include "fixtures.h"
#include "g29.h"
#include "hid_desc.h"
#include "test_util.h"

#define G29D g29_hid_report_desc, G29_HID_REPORT_DESC_LEN

static void test_g29_report_sizes(void) {
    CHECK_EQ(G29_HID_REPORT_DESC_LEN, 160);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_INPUT, 1), G29_INPUT_PAYLOAD_LEN * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_OUTPUT, 5), 31 * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_FEATURE, 3), 47 * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_FEATURE, 0xF0), 63 * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_FEATURE, 0xF1), 63 * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_FEATURE, 0xF2), 15 * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_FEATURE, 0xF3), 7 * 8);
    CHECK_EQ(hid_desc_report_bits(G29D, HID_RPT_FEATURE, 0xF4), 0);
    // Fixed replies cover the declared feature sizes.
    CHECK_EQ(sizeof(g29_feature_0x03), 47);  // = declared feature report 3 size
    CHECK_EQ(sizeof(g29_feature_0xf3), 7);
    CHECK_EQ(g29_feature_0xf3[1], 0x38);  // 56-byte pages
    CHECK_EQ(g29_feature_0xf3[2], 0x38);
}

// The report builder's bit positions must agree with what the descriptor declares.
static void test_g29_button_bits_match_descriptor(void) {
    static const g29_button_t order[14] = {
        G29_BTN_SQUARE, G29_BTN_CROSS, G29_BTN_CIRCLE, G29_BTN_TRIANGLE, G29_BTN_L1, G29_BTN_R1, G29_BTN_L2,
        G29_BTN_R2, G29_BTN_SHARE, G29_BTN_OPTIONS, G29_BTN_L3, G29_BTN_R3, G29_BTN_PS, G29_BTN_TOUCHPAD,
    };
    uint16_t ofs[16];
    CHECK_EQ(hid_desc_button_offsets(G29D, 1, ofs, 16), 14);
    for (int i = 0; i < 14; i++) {
        g29_report_t r;
        memset(&r, 0, sizeof(r));
        g29_set_button(&r, order[i], true);
        uint32_t v = 0;
        CHECK(hid_extract_bits(r.b, sizeof(r.b), ofs[i], 1, &v));
        CHECK_EQ(v, 1);
        CHECK(g29_get_button(&r, order[i]));
        // and nothing else in the first 7 bytes
        int bits = 0;
        for (int b = 0; b < 7 * 8; b++) {
            bits += (r.b[b / 8] >> (b % 8)) & 1;
        }
        CHECK_EQ(bits, 1);
    }
    hid_field_t hat;
    CHECK(hid_desc_find_usage(G29D, HID_RPT_INPUT, HID_PAGE_GENERIC_DESKTOP, HID_USAGE_GD_HAT, &hat));
    CHECK_EQ(hat.bit_offset, G29_OFS_BTN0 * 8);
    CHECK_EQ(hat.bit_size, 4);
    CHECK(hat.flags & HID_FLAG_NULL_STATE);
}

static void test_g29_neutral(void) {
    g29_report_t r;
    g29_report_neutral(&r);
    CHECK_EQ(r.b[G29_OFS_LX], 0x80);
    CHECK_EQ(r.b[G29_OFS_RY], 0x80);
    CHECK_EQ(g29_get_dpad(&r), G29_DPAD_NEUTRAL);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_WHEEL), 0x8000);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_THROTTLE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_BRAKE), 0xFFFF);
    CHECK_EQ(g29_get_u16(&r, G29_OFS_CLUTCH), 0xFFFF);
    CHECK(!g29_get_button(&r, G29_BTN_PS));
    // Offsets equal the reference g29_report_t (packed struct, 63 bytes).
    CHECK_EQ(G29_OFS_WHEEL, 4 + 3 + 35);
    CHECK_EQ(G29_OFS_BTN3, G29_OFS_GEARS + 3);
    CHECK_EQ(G29_OFS_BTN3 + 1 + 9, G29_INPUT_PAYLOAD_LEN);
}

static void test_parser_logical_max_rule(void) {
    // Logical Maximum 0x26 FF FF: unsigned 65535 when the minimum is >= 0 ...
    const uint8_t d1[] = { 0x05, 0x01, 0x09, 0x30, 0x15, 0x00, 0x26, 0xFF, 0xFF, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02 };
    hid_field_t f;
    CHECK(hid_desc_find_usage(d1, sizeof(d1), HID_RPT_INPUT, 1, HID_USAGE_GD_X, &f));
    CHECK_EQ(f.logical_max, 65535);
    CHECK_EQ(f.report_id, 0);
    // ... and signed when the minimum is negative.
    const uint8_t d2[] = { 0x05, 0x01, 0x09, 0x30, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02 };
    CHECK(hid_desc_find_usage(d2, sizeof(d2), HID_RPT_INPUT, 1, HID_USAGE_GD_X, &f));
    CHECK_EQ(f.logical_min, -32768);
    CHECK_EQ(f.logical_max, 32767);
    uint8_t payload[2] = { 0x00, 0x80 };  // -32768
    int32_t v;
    CHECK(hid_extract(payload, 2, &f, &v));
    CHECK_EQ(v, -32768);
    payload[0] = 0xFF;
    payload[1] = 0x7F;
    CHECK(hid_extract(payload, 2, &f, &v));
    CHECK_EQ(v, 32767);
    CHECK(!hid_extract(payload, 1, &f, &v));  // field beyond payload
}

static void test_parser_malformed_and_push_pop(void) {
    const uint8_t truncated[] = { 0x05, 0x01, 0x09, 0x30, 0x27, 0xFF };
    CHECK(!hid_desc_walk(truncated, sizeof(truncated), NULL, NULL));
    const uint8_t bad_pop[] = { 0xB4 };
    CHECK(!hid_desc_walk(bad_pop, sizeof(bad_pop), NULL, NULL));
    // Push/pop restores report size and count.
    const uint8_t pp[] = {
        0x05, 0x01, 0x75, 0x08, 0x95, 0x01, 0xA4,  // push (size 8, count 1)
        0x75, 0x10, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,  // 2 x 16 bits
        0xB4,  // pop
        0x09, 0x32, 0x81, 0x02,  // Z: 8 bits at 32
    };
    hid_field_t z;
    CHECK(hid_desc_walk(pp, sizeof(pp), NULL, NULL));
    CHECK(hid_desc_find_usage(pp, sizeof(pp), HID_RPT_INPUT, 1, HID_USAGE_GD_Z, &z));
    CHECK_EQ(z.bit_offset, 32);
    CHECK_EQ(z.bit_size, 8);
    // Long item is skipped.
    const uint8_t li[] = { 0xFE, 0x02, 0x10, 0xAA, 0xBB, 0x05, 0x01, 0x09, 0x30, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02 };
    hid_field_t x;
    CHECK(hid_desc_find_usage(li, sizeof(li), HID_RPT_INPUT, 1, HID_USAGE_GD_X, &x));
    CHECK_EQ(x.bit_offset, 0);
}

static void test_simagic_doc_layout_parses(void) {
    hid_field_t x, hat;
    CHECK(hid_desc_find_usage(SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), HID_RPT_INPUT, 1, HID_USAGE_GD_X, &x));
    CHECK_EQ(x.report_id, 1);
    CHECK_EQ(x.bit_offset, 0);
    CHECK_EQ(x.bit_size, 16);
    CHECK_EQ(x.logical_max, 65535);
    CHECK(hid_desc_find_usage(SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), HID_RPT_INPUT, 1, HID_USAGE_GD_HAT, &hat));
    CHECK_EQ(hat.bit_offset, 88);  // packet byte 12, low nibble
    CHECK_EQ(hid_desc_report_bits(SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), HID_RPT_INPUT, 1), 38 * 8);
    uint16_t ofs[128];
    CHECK_EQ(hid_desc_button_offsets(SIMAGIC_DOC_DESC, sizeof(SIMAGIC_DOC_DESC), 1, ofs, 128), 116);
    CHECK_EQ(ofs[0], 92);  // packet bit 100
    CHECK_EQ(ofs[115], 92 + 115);
    CHECK_EQ(ofs[116], 0xFFFF);
}

int main(void) {
    RUN(test_g29_report_sizes);
    RUN(test_g29_button_bits_match_descriptor);
    RUN(test_g29_neutral);
    RUN(test_parser_logical_max_rule);
    RUN(test_parser_malformed_and_push_pop);
    RUN(test_simagic_doc_layout_parses);
    TEST_MAIN_END();
}
