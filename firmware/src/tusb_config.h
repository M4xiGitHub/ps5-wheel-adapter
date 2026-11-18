// TinyUSB configuration.
// Device (rhport 0, native USB, to the PS5) and host (rhport 1+2, Pico-PIO-USB on
// GP0/GP1 and GP2/GP3). Values follow jfedor2/wheel-adapter r2026-11-07 except
// where noted.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifndef ADAPTER_BENCH
#define ADAPTER_BENCH 0
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

// ------------------------------------------------------------------ device
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_HID 1
#define CFG_TUD_HID_EP_BUFSIZE 64
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#if ADAPTER_BENCH
#define CFG_TUD_CDC 1  // bench build only: live diagnostics
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 2048
#define CFG_TUD_CDC_EP_BUFSIZE 64
#else
#define CFG_TUD_CDC 0
#endif

// ------------------------------------------------------------------ host
#define BOARD_TUH_RHPORT 1
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUH_RPI_PIO_USB 1

// Reference uses 512. 1024 leaves room for larger configuration descriptors of the
// composite rig devices until their real sizes are measured (costs 512 B of RAM).
#define CFG_TUH_ENUMERATION_BUFSIZE 1024

#define CFG_TUH_HUB 1         // the rig's internal 05E3:0610 hub
#define CFG_TUH_DEVICE_MAX 16 // 5 devices + hub need 6
#define CFG_TUH_HID 16        // HID interfaces across all devices
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#endif
