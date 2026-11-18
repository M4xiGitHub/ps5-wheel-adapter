// USB descriptors presented on the Pico's native USB port.
//
// Console build: identical to jfedor2/wheel-adapter r2026-11-07 (MIT,
// Copyright (c) 2026 Jacek Fedorynski): G29 046D:C260, one HID interface,
// IN 0x84 / OUT 0x03, 64 bytes, 5 ms.
// Bench build: the same HID interface plus a CDC-ACM diagnostic port. Not for the console.
#include <string.h>

#include "g29.h"
#include "tusb.h"

#if ADAPTER_BENCH
#define DEVICE_CLASS TUSB_CLASS_MISC
#define DEVICE_SUBCLASS MISC_SUBCLASS_COMMON
#define DEVICE_PROTOCOL MISC_PROTOCOL_IAD
#else
#define DEVICE_CLASS 0x00
#define DEVICE_SUBCLASS 0x00
#define DEVICE_PROTOCOL 0x00
#endif

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = DEVICE_CLASS,
    .bDeviceSubClass = DEVICE_SUBCLASS,
    .bDeviceProtocol = DEVICE_PROTOCOL,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = G29_VID,
    .idProduct = G29_PID,
    .bcdDevice = G29_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01,
};

#define EPNUM_HID_IN 0x84
#define EPNUM_HID_OUT 0x03

#if ADAPTER_BENCH
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define ITF_COUNT 3
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)
#else
#define ITF_COUNT 1
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#endif

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 200),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, G29_HID_REPORT_DESC_LEN, EPNUM_HID_OUT,
        EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
#if ADAPTER_BENCH
    TUD_CDC_DESCRIPTOR(1, 3, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "configuration length");

static const char* const string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },     // 0: English (0x0409)
    "Logitech",                        // 1: manufacturer
    "G29 Driving Force Racing Wheel",  // 2: product
#if ADAPTER_BENCH
    "PS5 adapter bench diagnostics",   // 3: CDC interface
#endif
};

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*) &desc_device;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t itf) {
    (void) itf;
    return g29_hid_report_desc;
}

static uint16_t desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char* str = string_desc_arr[index];
        chr_count = (uint8_t) strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint8_t) str[i];
        }
    }
    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
