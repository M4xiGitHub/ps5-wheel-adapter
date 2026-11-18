# GT7 with the GT Neo: buttons and force feedback

## Firmware variants

| UF2 | Force feedback | Use |
| --- | --- | --- |
| `ps5-adapter-console-<ver>.uf2` | on, capped at **30 %** | normal GT7 use, once T10–T12 have passed |
| `ps5-adapter-console-noffb-<ver>.uf2` | off, nothing is sent to the wheelbase | first PS5 acceptance test (T7), or any time you want no torque |
| `ps5-adapter-bench-<ver>.uf2` | on, capped, plus `--ffb-test` | computer only |

## Force feedback: what it does

```
PS5/GT7 ──G29 FFB commands──► adapter ──Simagic FFB packets──► wheelbase
          (Logitech classic,          (constant force, as in
           7-byte, report 5)           JacKeTUs/simagic-ff)
```

* **Translated:** steady forces (Logitech constant and zero-step variable force), which
  is what GT7 is reported to use (bilgon32, PS5/GT7). Download, play, stop and refresh
  commands are tracked per slot; the forces of the playing slots are summed.
* **Cap:** full G29 force from GT7 = **30 %** of the wheelbase's force range
  (`CFG_FFB_STRENGTH_PERCENT`). Every packet is clamped, whatever the console sends
  (tested with randomized command streams). Your base's own strength setting still
  applies on top.
* **Not translated (counted and logged):** spring, damper, friction, periodic effects,
  autocentre, rotation range, RPM LEDs, mode switches. If the log shows GT7 uses
  spring/damper, they can be added from that capture.
* **Never sent to the base:** any Logitech command, wheel initialization, or anything the
  base did not get from this translator.
* **Safety stops:**
  * at boot / wheelbase mount, every effect block is stopped before anything else;
  * a non-zero force held with no console FFB traffic for 2 s is stopped (dead-man);
  * the force stops when the console unconfigures or suspends the adapter, or the
    wheelbase disappears;
  * a firmware hang triggers the 2 s watchdog, and the reboot stops everything.
* **Emergency stop:** the wheelbase's power switch. Do not rely on unplugging USB.

Not yet verified on your hardware: that the base accepts the packets on this
interface, which way a positive force turns the wheel, and GT7's exact command mix.
That is what T10–T12 are for.

## Steering range

Set the wheelbase's rotation to **900°** in Simagic's software. The adapter maps the
base's full axis range onto the G29's, and a G29 is 900° lock to lock, so 900° keeps
GT7's steering 1:1. (Recommendation, not yet tested.)

## Buttons

The GT Neo has 10 RGB buttons, two 7-way switches, four rotary encoders, two thumb
encoders and paddle modules (Simagic manual). Its button **numbers** are not published,
so they are recorded with the wizard (bench build, rig on port B):

```
py tools\adapter_monitor.py --wizard
```

It asks for each control below in this order, suggests a GT Neo control, measures each
press, and adds a 50 ms hold only to controls that send short encoder pulses (so the
console cannot miss a detent). Paste the printed block into
`firmware/config/adapter_config.h`, or send it here for a build.

| G29 control | Suggested GT Neo control | Suggested GT7 use |
| --- | --- | --- |
| Cross | right-hand face button | confirm, PS5 user selection |
| Circle | left-hand face button | back |
| R1 / L1 | right / left shift paddle | shift up / down (paddles are R1/L1 on a G29; source 13 → R1 given) |
| Options | a button you won't hit by accident | pause |
| Triangle / Square | face buttons | change view / look back |
| Dial Enter | push of a 7-way switch | MFD select |
| Dial up / down | rotation of that 7-way switch | MFD value |
| Plus / minus | a rotary encoder | TCS or brake balance |
| L2 / R2 | lower paddles or face buttons | look left/right, nitrous, pit limiter |
| L3 / R3 | face buttons | spare |
| D-pad | the 7-way switch that reports as the hat (automatic) | MFD navigation, menus |
| PS | source button 6 (given) | PS button |

### Your recorded map (wizard, 2026-11-18, firmware 0.2.3)

| GT Neo source button | G29 control | | GT Neo source button | G29 control |
| --- | --- | --- | --- | --- |
| 6 | PS | | 18 | dial Enter |
| 10 | Cross (50 ms hold) | | 12 / 11 | dial up / down |
| 9 | Circle | | 33 / 34 | plus / minus |
| 13 / 14 | R1 / L1 (shift paddles) | | 17 / 23 | L2 / R2 |
| 7 | Triangle | | 38 / 37 | L3 / R3 |
| 4 | Square | | 29 / 30 / 28 / 25 | D-pad up / right / down / left |

Options (pause) is unassigned: button 18 was offered for both Options and dial Enter and is
dial Enter by choice. Name a free button to add Options.

GT7 functions are then assigned in GT7: Options → Controller Settings → Steering
Controller. A guide reports the default G29 layout as nearly complete and suggests moving
the MFD controls from the dial to L2/R2 ([overtake.gg](https://overtake.gg/news/gran-turismo-7-best-logitech-g29-wheel-settings.1884)).

## Force-feedback bring-up (do in order)

**T10 – bench, computer.** If your base keeps a strength setting from Simagic's software,
lower it for this first test. Bench firmware, rig on port B:
```
py tools\adapter_monitor.py --seconds 10
py tools\adapter_monitor.py --ffb-test 1000
py tools\adapter_monitor.py --ffb-test -1000
```
The first must show `ffb wheelbase output report OK`. Each test applies 3 % of the
base's range for 0.6 s; keep your hands on the wheel. Note which way it turns each time
(they must be opposite). If nothing moves, try `3000`, then send the output.

Result on your base (2026-11-18): a positive magnitude turns the wheel anticlockwise;
±9 % moved it about 7-8 degrees hands off.

**T11 – GT7, low speed.** Console firmware. In GT7, start with low force-feedback
strength. Drive slowly in a straight line, hands on the wheel: steer a little and let
go of the force. The wheel must **pull back towards centre**.
If it pushes **away** from centre or starts to oscillate, switch the wheelbase off,
set `CFG_FFB_INVERT 1` (or ask for that build), reflash and repeat.

**T12 – log.** After the session, `tools/pull_log.sh` (or `CURRENT.UF2`, see
`START_HERE.md`). The `ffb` lines list every kind of command GT7 sent, any untranslated
effect types, and the longest gap between console commands (to check the 2 s dead-man).
