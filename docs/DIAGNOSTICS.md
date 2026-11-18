# Diagnostics

## Why a flash log, read in BOOTSEL mode

On the PS5 there is no computer attached, the Pico is powered only by the console's USB
port, and unplugging it to bring it to a PC loses RAM. Options considered:

| Method | Verdict |
| --- | --- |
| RAM ring dumped after the session | lost at unplug: rejected |
| UART/CDC to a PC next to the console | needs extra hardware or a second USB function on the console-facing device (changes what the PS5 sees): rejected for the console build |
| Extra HID reports on the G29 interface | changes the console-facing descriptor: rejected |
| **Flash log, one slot per boot, read with `picotool` in BOOTSEL mode** | no extra hardware, console-facing USB unchanged, and the readout does not boot the firmware, so it cannot create a new "PC session" on top of the PS5 one: **chosen** |

Details:
* Last 1 MiB of flash, 4 slots × 256 KiB (> 16 000 records each, hours of normal logging).
  Each boot takes the slot after the newest one, erases it **before USB starts**
  (~0.6 s) and writes a header with a boot counter, build variant and reset cause.
  The three previous boots stay intact, so the PS5 session survives one or two later PC
  boots too.
* Records are 16 bytes; they go to a 512-entry RAM ring and are programmed into flash
  page by page (≤ 3 ms each). A full page is written at once, a partial tail after 2 s,
  urgent records (mounts, auth milestones, errors, PS button) after 50 ms.
  **Power-loss window: the last ≤ 2 s (≤ 50 ms for urgent records) before unplugging.**
* The image runs entirely from SRAM (`copy_to_ram`), so programming flash never stalls
  the PIO-USB frame timer or the native USB interrupt.
* If the ring overflows, the oldest records are dropped and counted (`HEARTBEAT3`);
  logging never waits.
* Flashing a new UF2 does not touch the log region.

A PC boot is never mistaken for a PS5 session: `adapter_log.py` reports per boot whether
the host ever asked for the auth reset report (GET 0xF3), which only the console does.

## Reading the log

```bash
tools/pull_log.sh
```

Unplug from the PS5, hold BOOTSEL while plugging into the computer, run the script. It
saves `logs/flashlog-*.bin`, lists all stored boots and decodes the newest console-build
boot (`--boot N` / `--all` for others), and saves the captured HID report descriptors to
`logs/descriptors/`.

## What each stage looks like in the log

| State | Records |
| --- | --- |
| Physical port connection | `PORT_EVENT port A/B ATTACH` |
| Enumeration | `DEV_MOUNT dev N VID:PID rhport hub_addr hub_port`, `DEV_ROLE … topology=OK/UNEXPECTED` |
| Interfaces | `HID_MOUNT idx itf len crc32`, full report descriptor in `HID_DESC_CHUNK` |
| Layout validation | `LAYOUT wheel/pedals: matches documented layout / DIFFERS / UNUSABLE` + `LAYOUT_FIELD` |
| Auth controller | `AUTH_DESC … VALID auth interface` |
| Live input | `FIRST_INPUT wheel/pedals`, `HEARTBEAT … wheel_rx pedal_rx` every 5 s |
| Upstream configuration | `USB_DEV CONFIGURED by host`, `CONSOLE_REQ host first GET/SET report …` |
| Console auth request | `auth CONSOLE_F3`, `CONSOLE_NONCE_PAGE … crc=ok`, `JOB_START` |
| Downstream signing | `DS_STEP_OK`, `DS_SIGNED after N ms`, `DS_PAGE_CHECK`, `DS_READY` |
| Response to console | `CONSOLE_F2 console told READY`, `CONSOLE_F1_PAGE`, `DELIVERED` |
| Failures | `DS_STEP_FAIL`, `DS_FAILED`, `DS_XFER_TIMEOUT`, `CONSOLE_NONCE_BAD`, `INPUT_REJECT`, `ERROR` |
| PS / user selection | `PS_BUTTON PRESSED` (actual acceptance is only visible on the TV) |
| Force feedback | `ffb wheelbase output report OK/UNUSABLE`, `ffb first console command …` (one per command kind GT7 sends), `ffb console uses … effects (not translated)`, `ffb constant force STARTED/stopped`, `ffb STALE`, `heartbeat ffb … max_gap` |
| Session length | last `HEARTBEAT up=…s` |

## Live view (bench build only)

`ps5-adapter-bench-*.uf2` adds a CDC serial port next to the same G29 HID interface. Use
it on the computer only:

```bash
.venv/bin/python tools/adapter_monitor.py
```

It prints the same records live plus a status line with raw steering/pedal values, pressed
source buttons and the resulting G29 fields. `--wizard` records the button mapping,
`--axes` measures ranges and directions, `--bootsel` reboots into the bootloader.

## LED (both builds)

| Pattern | Meaning |
| --- | --- |
| fast blink (4 Hz) | no host has configured the adapter yet |
| double blink | configured, but no validated auth controller (Hori missing, or its descriptor lacks F0–F3) |
| slow blink (1 Hz) | auth controller ready; waiting for / running a console auth round |
| solid | the console has received a complete signature since its last auth reset |
