// User-facing adapter configuration. Rebuild and reflash after editing.
//
// Status of each setting:
//   [spec]     given by the owner
//   [measure]  must be confirmed on the real rig (tools/adapter_monitor.py, docs/TEST_PLAN.md)
#ifndef ADAPTER_CONFIG_H
#define ADAPTER_CONFIG_H

#include "g29.h"

// ------------------------------------------------------------------ buttons
// { Simagic source button (1-based HID Button usage), G29 control [, min hold ms] }
// Several sources may drive the same target (logical OR). The optional third value
// keeps a short press (rotary encoder pulse) visible to the console for that long;
// the wizard suggests it only for controls it measured as short pulses.
//
// Only the [spec] entries are known. Run `tools/adapter_monitor.py --wizard` with the
// bench build: it asks for each GT7 function in turn, suggests a GT Neo control for
// it, and prints a replacement block. See docs/GT7_SETUP.md.
#define CFG_BUTTON_MAP                 \
    { 6, G29_BTN_PS, 0 },  /* [spec] */   \
    { 13, G29_BTN_R1, 0 }, /* [spec] shift up */

// ------------------------------------------------------------------ axes
// Steering: Simagic Generic-Desktop X, scaled linearly from its logical range to
// 0..65535 (0 = full left on the G29). Set to 1 if the measured direction is reversed.
#define CFG_STEER_INVERT 0  // [measure]

// Pedals: VRS Z -> throttle, VRS Y -> brake [spec]. The G29 reports 0xFFFF released
// and 0 fully pressed. CFG_PEDALS_RISE_WHEN_PRESSED = 1 means the VRS value grows as
// the pedal is pressed (logical minimum = released).
#define CFG_PEDALS_RISE_WHEN_PRESSED 1  // [measure]

// Clutch stays released (0xFFFF) until its source and behaviour are verified.
#define CFG_CLUTCH_ENABLED 0

// ------------------------------------------------------------------ force feedback
// Enabled on request [spec]. The build can override it: the console-noffb UF2 is
// built with ADAPTER_FFB=0 for tests without motor torque.
#ifdef ADAPTER_FFB
#define CFG_FFB_ENABLED ADAPTER_FFB
#else
#define CFG_FFB_ENABLED 1
#endif

// Hard cap: full G29 force from the console = this percentage of the wheelbase's
// force range. Enforced on every packet; nothing can exceed it. [spec: 30]
#define CFG_FFB_STRENGTH_PERCENT 30

// Direction: set to 1 if, in GT7, the wheel pushes AWAY from centre when driving
// straight (it must pull back towards centre). See docs/GT7_SETUP.md. [measure]
#define CFG_FFB_INVERT 0

// Dead-man: a non-zero force held with no console FFB traffic for this long is
// stopped. The logged maximum console gap shows whether it needs adjusting.
#define CFG_FFB_STALE_MS 2000

#endif
