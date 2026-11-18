# PS5 sim-racing adapter (Raspberry Pi Pico)

Firmware for a Raspberry Pi Pico (RP2040) that is meant to let a Simagic wheel + VRS
DirectForce Pro pedals rig drive Gran Turismo 7 on a PS5 directly, without a PC or Remote
Play. Towards the console the Pico presents itself as a **Logitech G29 (PS4 mode)**, and it
forwards the console's controller-authentication challenge to a licensed **Hori**
controller plugged into its second USB host port.

```
Hori 0F0D:0087 ──► port A: GP0 D+ / GP1 D-
Rig cable ───────► port B: GP2 D+ / GP3 D- (22 Ω) ──► hub 05E3:0610
                         ├─ Simagic 0483:0522      steering, hat, buttons
                         ├─ VRS 0483:A3BE          throttle (Z), brake (Y)
                         ├─ Stream Deck 0FD9:0063  (kept enumerated)
                         └─ HID 8087:1024          (kept enumerated)
Pico Micro-USB ──► PS5 (or a computer for testing)
```

> **Status: hardware testing has not started.** The firmware builds and its automated
> tests pass, but it has not yet run on the real adapter, rig or PS5. PS5 / GT7
> compatibility is **not** claimed. Progress is tracked in [docs/TEST_PLAN.md](docs/TEST_PLAN.md).

| Stage | Status |
| --- | --- |
| Reproducible build with pinned dependencies | done (verified on macOS arm64, clean rebuild gives identical UF2s) |
| Automated host tests (HID parser, G29 layout, mapping, routing, auth sequencing and failures, flash log, tools) | pass |
| Console-facing USB identity vs a hardware-tested reference | byte-identical where it matters, see [docs/CONSOLE_BEHAVIOR.md](docs/CONSOLE_BEHAVIOR.md) |
| Hori signs, rig layouts, auth through the adapter on a PC (T1–T5) | not run yet |
| PS5 acceptance, PS/user selection, GT7 controls, 15 min, repeated starts (T7–T9) | not run yet |
| Force feedback to the Simagic base, capped at 60 % (T10–T12) | works on the owner's base and in GT7 (curbs); 0.2.4 removes a timeout that cut held forces; see [docs/GT7_SETUP.md](docs/GT7_SETUP.md) |

## Setup

There are two ways in. Most people only need the first.

### A. Flash and test – any OS, no build tools

Everything needed on the computer next to the hardware is in one zip:
[`dist/ps5-adapter-kit-0.2.4.zip`](dist/ps5-adapter-kit-0.2.4.zip) (firmware, diagnostic
tools, test docs). Download it, unzip, and follow its `START_HERE.md`. In short:

1. Install Python 3 (Windows: tick "Add python.exe to PATH").
2. In the unzipped folder: `py -m pip install -r requirements-tools.txt`
   (macOS/Linux: `python3 -m pip install -r requirements-tools.txt`, ideally in a venv).
3. Check the firmware checksums against `firmware/SHA256SUMS`.
4. Run the bench steps in `START_HERE.md`: Hori signing check, flash the **bench**
   firmware, enumeration and input check, button-mapping wizard, auth through the adapter,
   power check.

The firmware files are also directly in [`dist/`](dist/):

| File | Use |
| --- | --- |
| `ps5-adapter-console-<ver>.uf2` | **for the PS5**, force feedback on (60 % cap) |
| `ps5-adapter-console-noffb-<ver>.uf2` | for the PS5, no torque to the wheelbase (first acceptance test) |
| `ps5-adapter-bench-<ver>.uf2` | for a computer only: same G29 interface plus a USB serial diagnostics port. **Never on the console.** |
| `SHA256SUMS` | checksums of the UF2s and the kit |
| `BUILDINFO.txt` | compiler, dependency commits, image sizes |

**Flashing:** hold the Pico's BOOTSEL button, plug it into the computer, release; copy the
UF2 onto the `RPI-RP2` drive. The Pico reboots by itself. Flashing never erases the
diagnostic log.

### B. Build from source – macOS or Linux

Verified on macOS (Apple silicon). The Linux path in the scripts is not yet tested; on
Windows use WSL2 (also untested).

Prerequisites:

* **macOS:** Xcode Command Line Tools (`xcode-select --install`), Homebrew, then
  `brew install libusb pkgconf`
* **Debian/Ubuntu:** `sudo apt install build-essential git curl python3-venv libusb-1.0-0-dev pkg-config`

```bash
git clone https://github.com/M4xiGitHub/ps5-wheel-adapter.git
```

```bash
cd ps5-wheel-adapter
```

```bash
scripts/setup.sh
```

```bash
scripts/build.sh
```

```bash
scripts/make_kit.sh
```

`setup.sh` installs everything inside the project folder, nothing system-wide:

| Path | Content |
| --- | --- |
| `.tools/arm-gnu-toolchain/` | Arm GNU Toolchain 14.2.Rel1, SHA-256 verified |
| `.venv/` | Python venv with pinned cmake, ninja, hidapi, pyserial, cryptography ([requirements.txt](requirements.txt)) |
| `deps/` | pico-sdk, TinyUSB, Pico-PIO-USB, picotool at the exact commits in [deps.lock](deps.lock) |
| `.tools/picotool/` | picotool 2.3.1 built from source |

`build.sh` runs the host tests, builds both firmware variants and writes `dist/`.
`make_kit.sh` packs the kit zip. If you move the project folder, rerun `setup.sh` and
`build.sh`; they recreate the parts that store absolute paths.

## Configure

[firmware/config/adapter_config.h](firmware/config/adapter_config.h):

* Button map: source button 6 → PS and 13 → R1 are given. Record the rest with
  `tools/adapter_monitor.py --wizard` (bench firmware); it prints the block to paste.
* Steering and pedal direction (to confirm with `tools/adapter_monitor.py --axes`).
* Clutch: disabled until verified.
* Force feedback: on, capped at 60 % (`CFG_FFB_STRENGTH_PERCENT`), direction
  `CFG_FFB_INVERT`. Forces are held until GT7 changes them, as on a G29. Only translated Simagic packets
  reach the base, never Logitech commands. Details: [docs/GT7_SETUP.md](docs/GT7_SETUP.md).

Rebuild and reflash after editing.

## Diagnostics

| Tool | Purpose |
| --- | --- |
| `tools/hid_probe.py` | list, dump and watch HID devices on the computer (read-only) |
| `tools/auth_probe.py --target hori\|adapter` | run one signing exchange from the computer and verify the returned RSA signature |
| `tools/adapter_monitor.py` | live view of the bench firmware: events, raw and mapped inputs, `--wizard`, `--axes`, `--bootsel` |
| `tools/adapter_log.py` | decode the flash log from a picotool dump or `CURRENT.UF2` |
| `tools/pull_log.sh` | macOS/Linux helper: read the flash log with picotool in BOOTSEL mode and decode it |

On the PS5 there is no computer attached, so the firmware logs to the Pico's flash, one
slot per boot. After a console session, unplug the adapter, plug it into the computer
in BOOTSEL mode (the firmware does not start, so the session is not overwritten) and read
the log. Details and LED codes: [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

## Documents

* [docs/GT7_SETUP.md](docs/GT7_SETUP.md): GT Neo button plan for GT7, force feedback and its safety limits
* [docs/TEST_PLAN.md](docs/TEST_PLAN.md): physical tests T1–T12, pass criteria, results log
* [docs/HARDWARE.md](docs/HARDWARE.md): wiring and the power checks P1–P4
* [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md): flash log design, record meanings, LED codes
* [docs/CONSOLE_BEHAVIOR.md](docs/CONSOLE_BEHAVIOR.md): what the PS5 sees, compared with the reference adapter
* [docs/EVIDENCE.md](docs/EVIDENCE.md): reference projects, dependency choice, verified USB-stack behaviour

## Layout

```
firmware/src/      main.c  auth.c  input_map.c  devices.c  hid_desc.c  diag.c  g29.c
                   usb_descriptors.c  crc32.c  tusb_config.h
firmware/config/   adapter_config.h
tests/             host unit tests (C, with ASan/UBSan) and tool tests (Python)
tools/             diagnostic tools
scripts/           setup.sh  build.sh  make_kit.sh
dist/              built firmware, checksums, kit
deps.lock          pinned source dependencies
```

## Credits and licence

The G29 descriptors, fixed feature replies and the authentication sequence follow
[jfedor2/wheel-adapter](https://github.com/jfedor2/wheel-adapter) (MIT), whose author
reports PS5/GT7 operation with other wheels. Pico SDK, TinyUSB and Pico-PIO-USB are used
under their own licences. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

No licence has been chosen for this project's own code yet.
