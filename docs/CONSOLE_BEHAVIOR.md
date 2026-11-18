# Console-facing behaviour vs the hardware-tested reference

Reference: **jfedor2/wheel-adapter r2026-11-07** (commit `87e03d1`, MIT). Its author
reports PS5/GT7 operation with a Driving Force Pro and Razer Raion / MagicBoots /
PXN N5 authentication. bilgon32/ps5-wheel-adapter (firmware-v2026.10.20) uses the
same descriptors and auth sequence and reports GT7 operation with a *Hori Wired
Controller Light for PS4*. Neither has been tested with this rig, this Hori
(0F0D:0087) or two host ports carrying a 4-device hub.

`tests/test_hid_g29.c` and the comparison script in the build log check the bytes below.

## Identical to the reference

| Item | Value |
| --- | --- |
| Device descriptor | USB 2.00, class 0, EP0 64, **046D:C260**, bcdDevice 0x0100, iManufacturer "Logitech", iProduct "G29 Driving Force Racing Wheel", no serial |
| Configuration | 1 interface, self-powered attribute, 200 mA |
| HID interface | protocol none, IN 0x84, OUT 0x03, 64 bytes, 5 ms |
| Report descriptor | 160 bytes, byte-identical |
| Input report | ID 1 + 63 bytes; same field offsets (wheel @42, throttle @44, brake @46, clutch @48) |
| Send policy | only when the report changes |
| GET 0x03 | the same 47-byte reply |
| GET 0xF3 | `00 38 38 00 00 00 00`, answered locally, resets "signature ready" |
| Auth sequence downstream | GET F3, SET F0 ×5, GET F2 until byte 2 = 0, GET F1 ×19, one transfer at a time |
| GET 0xF2 | `nonce_id, 0x10` while busy / `nonce_id, 0x00` when ready |
| Dispatch | by report ID regardless of report type, as the reference |
| Console output (FFB) | accepted on OUT EP / SET_REPORT; here counted and dropped |
| Clock / USB stack | 192 MHz, VREG 1.15 V; pico-sdk 2.3.1, jfedor2 TinyUSB `c7d880e`, jfedor2 Pico-PIO-USB `fc89362` |
| Host ports | port A root on GP0 via `tuh_configure`, port B added with `pio_usb_host_add_port(2, DPDM)` after `tusb_init()` |

## Deliberate differences

| Item | Reference | Here | Why / risk |
| --- | --- | --- | --- |
| F0 pages to the auth controller | 56 data bytes re-packed, CRC bytes left zero | the console's 63-byte page forwarded verbatim (CRC included) | the controller sees exactly what a console sends it; strictly closer to a real console |
| F1 pages to the console | `nonce_id, part, 0`, 56 bytes, last 4 bytes = stale buffer | the controller's page verbatim (its own CRC32) | same first 59 bytes; the 4 trailing bytes become a valid CRC instead of leftovers |
| F2 trailing bytes | stale buffer | zeros + CRC32 over ID + 11 bytes | replaces undefined bytes |
| F1 page counter | not reset on F3 / new nonce | reset on F3 and on nonce page 0 | avoids serving a misaligned page after an aborted round |
| Auth controller selection | any non-wheel HID device | only 0F0D:0087 **and** a report descriptor with feature reports F0/F1/F2/F3 | the rig hub carries 4 other HID devices; they must never receive auth traffic |
| Downstream failure handling | `busy` set even if the submit failed (can stall forever) | submit result checked, 3 retries per step, 15 s signing timeout, 7 s transfer backstop, stale completions ignored | the console simply retries its round; we must not wedge |
| Neutral input before rig reports | all zero (wheel full left, pedals "pressed", dpad up) | wheel 0x8000, pedals/clutch 0xFFFF, dpad 8 (same as bilgon32) | safe neutral |
| GET input report 1 | stale buffer | current report | defined content |
| Unknown GET IDs | `reqlen` stale bytes | `reqlen` zero bytes | defined content |
| Force feedback / wheel init | forwarded as-is to Logitech wheels; Logitech init commands sent | steady forces translated to Simagic packets (simagic-ff protocol) capped at 60 %; everything else dropped; no Logitech command ever reaches the base. `console-noffb` build sends nothing | a Simagic DD base does not speak the Logitech protocol; see docs/GT7_SETUP.md |
| Diagnostics | UART printf on GP16 | flash log (both builds), CDC port (bench build only) | console sessions must be readable after power loss |
| Firmware image | runs from flash (XIP) | `copy_to_ram` | flash log writes cannot stall USB interrupts |
| Watchdog | none | 2 s | a hang becomes a logged reboot instead of a dead controller |
| UART / TinyUSB BSP | `board_init()` + `stdio_init_all()` | not used | GP0/GP1 are host port A; no console-facing effect |

The bench build differs further (composite HID + CDC, device class EF/02/01) and must
not be used on the console.
