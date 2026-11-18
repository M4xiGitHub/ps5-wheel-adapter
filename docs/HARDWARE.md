# Hardware: wiring, power and checks

## Wiring (Raspberry Pi Pico, RP2040, official pinout)

| Signal | Pico GPIO | Physical pin | Note |
| --- | --- | --- | --- |
| Host A D+ (Hori) | GP0 | 1 | direct |
| Host A D- (Hori) | GP1 | 2 | direct; GP0/GP1 are also the SDK's default UART pins, so the firmware moves the UART to GP16/GP17 and never enables it |
| Host B D+ (rig cable) | GP2 | 4 | 22 Ω series |
| Host B D- (rig cable) | GP3 | 5 | 22 Ω series |
| Host grounds | GND | 38 | common |
| Host VBUS (both ports) | VBUS | 40 | = Micro-USB VBUS from the PS5/PC |
| VSYS | – | 39 | unused |
| Status LED | GP25 | – | on-board |
| BOOTSEL | – | button | hold while plugging in for the USB bootloader |

Pico-PIO-USB requires D- = D+ + 1 (`PIO_USB_PINOUT_DPDM`); both ports satisfy this.
The firmware registers port A as root port 0 (TinyUSB rhport 1) and port B as root
port 1 (rhport 2); the logs name them "A (GP0/GP1)" and "B (GP2/GP3)".

Observed on 2026-11-18: the rig's hub enumerated on port A (rhport 1, GP0/GP1) and the
Hori on port B, i.e. swapped relative to the table. The firmware does not depend on which
port a device uses (it identifies devices by VID/PID and descriptor), so no change is
needed; the logs flag it as "topology=UNEXPECTED" only for information.

The 22 Ω resistors on port B only are not treated as a problem: there is no measurement
indicating one. They are listed so a port-specific error rate (if ever logged) can be
related to them.

## Power: what the circuit implies

* Pin 40 (VBUS) is wired directly to the Micro-USB VBUS. Everything on both host ports
  is therefore powered **by the PS5's (or the PC's) USB port** through the Pico's
  Micro-USB connector and the cable. There is no switch or current limit on the Pico.
* When the Micro-USB cable is unplugged the Pico is **unpowered** (unless something
  back-feeds pin 40 — see check P3). RAM contents are lost; that is why the
  diagnostic log is written to flash (docs/DIAGNOSTICS.md).
* The wheelbase has its own supply. Whether the rig's internal hub (05E3:0610) and the
  pedals/Stream Deck draw from the USB VBUS or from the rig's supply is unknown.

## Checks before any power change (none proposed now)

No power modification is part of the design. These measurements decide whether one is
ever needed; record the values in docs/TEST_PLAN.md.

| ID | Measure | How | Healthy |
| --- | --- | --- | --- |
| P1 | Current drawn by the adapter + Hori + rig from the console port | USB-A inline meter between PS5 and the adapter's cable, rig powered, in GT7 | well below the port's rating; note the value |
| P2 | VBUS at the host ports under load | multimeter VBUS–GND on the port-B connector while driving in GT7 | ≥ 4.75 V (USB 2.0 device minimum is 4.40 V at the device) |
| P3 | Back-feed from the rig | Micro-USB unplugged, rig and wheelbase PSU on, measure VBUS (pin 40) to GND | ≈ 0 V. A clearly non-zero value means the rig drives VBUS backwards into the PS5/PC rail: stop and report it before any further console test |
| P4 | Brown-out symptoms | `reset=` field of each BOOT record (tools/pull_log.sh) | resets only at plug-in (POR), never mid-session |
