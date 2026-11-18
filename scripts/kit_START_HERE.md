# PS5 adapter kit – what to do on the PC next to the hardware

This kit holds only what the hardware PC needs: the two firmware files, the diagnostic
tools and the test docs. Nothing in it changes your PS5 or rig settings.
**Stop after step 6 and send the outputs back** – the console firmware needs the button
map from step 4 first.

Commands are for Windows (`py`). On macOS/Linux use `python3` and `/` instead of `\`.

## 0. One-time setup
1. Install Python 3 from python.org (tick "Add python.exe to PATH").
2. Open a terminal in this folder and run:
   ```
   py -m pip install -r requirements-tools.txt
   ```
3. Check the firmware files against `firmware\SHA256SUMS`:
   ```
   certutil -hashfile firmware\ps5-adapter-bench-VERSION.uf2 SHA256
   certutil -hashfile firmware\ps5-adapter-console-VERSION.uf2 SHA256
   ```

## 1. Does the Hori sign? (only the Hori, plugged straight into this PC)
```
py tools\hid_probe.py
py tools\auth_probe.py --target hori
```
Wanted: `nonce signature: VALID`.

## 2. Flash the BENCH firmware
Hold the Pico's BOOTSEL button, plug its Micro-USB into this PC, release. Copy
`firmware\ps5-adapter-bench-VERSION.uf2` onto the `RPI-RP2` drive; the Pico reboots.
Unplug it again. (The bench firmware is for this PC only, never for the PS5.)

## 3. Bench test from power-up
Hori into port A, the rig's cable into port B, wheelbase powered. **Then** plug the Pico
into this PC.
```
py tools\adapter_monitor.py --seconds 30
```
While it runs: turn the wheel, press each pedal, press a few wheel buttons.

## 4. Ranges and button map
```
py tools\adapter_monitor.py --axes --seconds 20
py tools\adapter_monitor.py --wizard
```
`--axes`: wheel lock to lock, then each pedal fully down and up.
`--wizard`: press the wheel button you want for each PS control (Enter skips one).
At least Cross is needed to pick your user on the PS5.

## 5. Signing through the adapter (same connections as step 3)
```
py tools\auth_probe.py --target adapter
```
Wanted: `nonce signature: VALID`.

## 6. Power check before the first console test (multimeter)
Pico's Micro-USB **unplugged**, rig connected to port B and wheelbase on: measure DC
voltage between Pico pin 40 (VBUS) and pin 38 (GND). Expected about 0 V. Note the value.

➜ Send the output of steps 1, 3, 4, 5 and the step-6 voltage.

---

## Later: PS5 test and reading its log
The console firmware (`ps5-adapter-console-…uf2`) and the console tests are in
`docs\TEST_PLAN.md` (T7–T9). After a PS5 session the log is read like this:

1. Unplug the adapter from the PS5. Hold BOOTSEL, plug it into this PC, release. The
   firmware does not start in this mode, so the PS5 session is not overwritten.
2. Easiest (untested on real hardware so far): copy `CURRENT.UF2` from the `RPI-RP2`
   drive into this folder and run
   ```
   py tools\adapter_log.py CURRENT.UF2 --descriptors descriptors
   ```
3. If it reports that the UF2 has no log region, use picotool 2.3.1
   (https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/picotool-2.3.1-x64-win.zip):
   ```
   picotool save -r 0x10100000 0x10200000 flashlog.bin
   py tools\adapter_log.py flashlog.bin --descriptors descriptors
   ```
   On Windows, if picotool finds no device, install the WinUSB driver for
   "RP2 Boot (Interface 1)" with Zadig, then retry.
4. `picotool reboot` (or unplug/replug) starts the firmware again.

LED meaning: fast blink = not configured by a host; double blink = no valid Hori;
slow blink = waiting for/doing authentication; solid = console got a complete signature.
