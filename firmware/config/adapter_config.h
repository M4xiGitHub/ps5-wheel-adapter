// User-facing adapter configuration. Rebuild and reflash after editing.
//
// Status of each setting:
//   [spec]     given in the project brief
//   [measure]  must be confirmed on the real rig (tools/adapter_monitor.py, docs/TEST_PLAN.md)
#ifndef ADAPTER_CONFIG_H
#define ADAPTER_CONFIG_H

#include "g29.h"

// ------------------------------------------------------------------ buttons
// Simagic source button number (1-based HID Button usage) -> G29 control.
// Several sources may drive the same target (logical OR).
// Only the two [spec] entries are known. Run `tools/adapter_monitor.py --wizard`
// with the bench build to record the rest (it prints a replacement block).
#define CFG_BUTTON_MAP                 \
    { 6, G29_BTN_PS },  /* [spec] */   \
    { 13, G29_BTN_R1 }, /* [spec] */

// ------------------------------------------------------------------ axes
// Steering: Simagic Generic-Desktop X, scaled linearly from its logical range to
// 0..65535 (0 = full left on the G29). Set to 1 if the measured direction is reversed.
#define CFG_STEER_INVERT 0  // [measure]

// Pedals: VRS Z -> throttle, VRS Y -> brake [spec]. The G29 reports 0xFFFF released
// and 0 fully pressed. CFG_PEDALS_RISE_WHEN_PRESSED = 1 means the VRS value grows as
// the pedal is pressed (logical minimum = released).
#define CFG_PEDALS_RISE_WHEN_PRESSED 1  // [measure]

// Clutch stays released (0xFFFF) until its source and behaviour are verified [spec].
#define CFG_CLUTCH_ENABLED 0

// ------------------------------------------------------------------ outputs to rig
// Force feedback / motor commands to the rig: disabled for milestone 1 [spec].
// The console's output reports are counted and dropped. No Logitech init commands
// are ever sent downstream.
#define CFG_FFB_ENABLED 0

#endif
