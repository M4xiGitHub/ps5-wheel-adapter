# Physical test plan and results

Run in order; each stage isolates one link of the chain. Commands run from the project
folder. "You" = steps that need hands on the hardware; the rest can be run by the
assistant when the device is plugged into the development computer.

Milestone 1 is met only when **T7, T8 and T9 pass on the real PS5**. A green build, Mac
HID input, Hori detection or a delivered signature are intermediate results.

## Results log

| Test | Date | Firmware (SHA-256 prefix) | Result | Notes |
| --- | --- | --- | --- | --- |
| Automated host tests | 2026-11-18 | v0.2.0 console e24fe9f9… | pass (6/6 suites incl. FFB cap) | not hardware evidence |
| T1 Hori signs | 2026-11-18 | console 0.2.3 (via adapter) | pass | covered by T5: the Hori 0F0D:0087 signs; signature verifies against its embedded 2048-bit key |
| T2 Rig descriptors | 2026-11-18 | bench 0.1.0 e1e4edee… | pass (via adapter) | Simagic 185 B and VRS 80 B descriptors captured; both match the brief's layouts; now test fixtures. Hat is 4-way (logical 0..3) -> fixed in 0.2.1 |
| T3 Enumeration on both ports | 2026-11-18 | bench 0.1.0 e1e4edee… | pass, with note | macOS host. Hori validated (F0/F1/F2/F3); hub + all 4 rig devices mounted; wheel ~1000 reports/s, pedals ~70/s, no rejects/drops. **Rig is on port A (GP0/GP1), Hori on port B** – swapped vs the plan; works because roles come from VID/PID + descriptors |
| T4 Live input + mapping | 2026-11-18 | bench 0.2.1 | map recorded, ranges pending | wizard on the GT Neo (second run); 18 = dial Enter by owner's choice, Options unassigned; in config since 0.2.3. steering: anticlockwise lowers the reading, so 0 = left as on the G29 (CFG_STEER_INVERT 0 confirmed); pedals 0 at rest, rise when pressed (CFG_PEDALS_RISE_WHEN_PRESSED 1 confirmed) |
| T5 Auth through adapter (PC) | 2026-11-18 | console 0.2.3 d165d8ed… | pass | macOS: F3 answered locally, nonce forwarded, Hori ready after ~1.1 s, 19 pages returned; nonce signature VALID (RSA-PSS/SHA-256, salt 32, 2048-bit, e=65537). Hori F1 pages lack the console-style CRC32, as on the PS5 |
| T6 Power checks P1–P4 | | | pending | |
| T7 PS5 accept + PS/user select + GT7 | | console 0.1.0 2aa676cb… | log evidence, on-screen result to confirm | flash log boot #8: configured by the PS5, PS (source 6) pressed 23x; owner to confirm what the TV showed and GT7 control |
| T8 15 min continuous in GT7 | | console 0.1.0 2aa676cb… | log: 2 h 05 min, auth OK every 57 s | 131 rounds, 130 delivered, 0 failures, Hori signs in ~939 ms, all 655 console nonce pages CRC-valid; 12 061 console output reports; ended by console USB suspend |
| T9 Repeat startups ×3 | | | pending | |
| T10 FFB bench (format, direction) | 2026-11-18 | bench 0.2.3 | pass (transport, cap, stops); physical side to confirm | output report 1 (20 B) accepted; boot sent 6 stop packets; tests capped to 30 %: ±1000 raw -> ±300 (3 %) moved the wheel ~1-2 deg, ±3000 raw -> ±900 (9 %) moved it −514 / +565 counts (~7-8 deg at 900 deg), hands off. **Positive Simagic magnitude turns the wheel anticlockwise** (owner observed; steering reading falls). GT7 centring (T11) confirms CFG_FFB_INVERT |
| T11 FFB in GT7, low speed, centring | 2026-11-18 | console 0.2.3 d165d8ed… | partial | owner: FFB present (curb rumble) but very weak at 30 % and "random", none on impacts. Cause found: the 2 s traffic timeout cut held forces and cleared the playing state (GT7 sends only on change; 0.1.0 log shows >2 s gaps while driving). Removed in 0.2.4, cap raised to 60 %. Centring direction still to confirm |
| T12 FFB log review (command mix, gaps) | | | pending | |

## T1 – Does this Hori sign? (computer only)
You: plug the Hori straight into the computer.
```bash
.venv/bin/python tools/hid_probe.py
.venv/bin/python tools/auth_probe.py --target hori
```
Pass: `nonce signature: VALID`. Record the product string, F2 latency and serial.
Fail modes: `not found` (permission / mode), no F0 report in the descriptor (wrong mode
or a model without the auth chip), signature NOT VALID.

## T2 – Rig descriptors and raw layout (computer only, optional but quick)
You: plug the rig's USB cable straight into the computer, wheelbase powered.
```bash
.venv/bin/python tools/hid_probe.py --dump logs/rig-descriptors
.venv/bin/python tools/hid_probe.py --watch 0483:0522     # press button 6, then 13, turn the wheel
.venv/bin/python tools/hid_probe.py --watch 0483:A3BE     # press throttle, brake, clutch
```
Pass: Simagic button 6 toggles packet bit 105, button 13 bit 112; steering changes bytes
1–2; VRS throttle changes bytes 5–6 (Z), brake bytes 3–4 (Y).

## T3 – Adapter enumeration from boot (bench build, computer)
You: flash `dist/ps5-adapter-bench-<ver>.uf2` (hold BOOTSEL, plug in, copy to RPI-RP2).
Connect the Hori to port A and the rig to port B **before** plugging the adapter into the
computer.
```bash
.venv/bin/python tools/adapter_monitor.py --seconds 20
```
Pass: port A and B ATTACH; six `DEV_MOUNT` lines with `topology=OK` (Hori on A, hub on B,
four devices behind it); `AUTH_DESC … VALID`; wheel and pedal `LAYOUT` usable (record
"matches"/"DIFFERS" and the fields); `FIRST_INPUT` for wheel and pedals; no `ERROR`.

## T4 – Live inputs, directions and button mapping (bench build)
```bash
.venv/bin/python tools/adapter_monitor.py --axes --seconds 20   # wheel lock to lock, each pedal fully
.venv/bin/python tools/adapter_monitor.py --wizard              # press the button for each G29 control
```
Set `CFG_STEER_INVERT`, `CFG_PEDALS_RISE_WHEN_PRESSED` and `CFG_BUTTON_MAP` in
`firmware/config/adapter_config.h` from the results, run `scripts/build.sh`, reflash bench.
Pass: in the monitor's status line, G29 wheel goes 0 (full left) → 65535 (full right),
throttle/brake go 65535 (released) → 0 (pressed), source button 6 shows `PS`, 13 shows
`R1`, and every mapped button shows its G29 name. No smoothing or deadzone is applied.

## T5 – Auth path through the adapter (bench or console build, computer)
Same connections as T3.
```bash
.venv/bin/python tools/auth_probe.py --target adapter
```
Pass: `VALID`, F1 pages without CRC problems. This exercises console-side reports,
forwarding to the Hori and the reply path, without the PS5.

## T6 – Power checks
See docs/HARDWARE.md, P1–P4. Do P3 before the first console test.

## T7 – PS5 acceptance, PS/user selection, GT7 controls (console build)
You:
1. Flash `dist/ps5-adapter-console-noffb-<ver>.uf2` for this first run (no torque to the
   wheelbase); switch to the FFB build after T10.
2. Hori on port A, rig on port B, wheelbase powered, then plug the adapter into the PS5
   (PS5 already on, at the home screen).
3. Watch the LED: fast blink → slow blink → solid within ~10 s.
4. Press the wheel button you mapped to PS (source button 6). Note what the TV shows.
5. Select your user with the D-pad / Cross mapping.
6. Start GT7, open Options → Controller settings → Steering controller: check steering,
   throttle, brake and the mapped buttons. Drive one lap.
7. Unplug, run `tools/pull_log.sh`.
Pass: user selection works, GT7 drives, and the log shows `CONFIGURED`, a `CONSOLE_F3`,
`DS_READY`, `DELIVERED`, `PS_BUTTON PRESSED`, no `DS_FAILED`.

## T8 – 15 minutes of continuous driving
Keep driving in GT7 for at least 15 minutes (past the 8-minute timeout of
unauthenticated controllers). Pass: no disconnect/"controller not supported" message;
log covers ≥ 900 s with heartbeats, no `DS_FAILED`, `delivered` ≥ 1.

## T10–T12 – Force feedback
Procedure and pass criteria: [GT7_SETUP.md](GT7_SETUP.md#force-feedback-bring-up-do-in-order).
Emergency stop is the wheelbase power switch.

## T9 – Repeat startup
Three times each: (a) PS5 fully off → power on with everything connected; (b) unplug and
replug the adapter at the home screen. Pass: T7 steps 3–6 succeed every time; each boot
appears in the log with `DELIVERED`.
