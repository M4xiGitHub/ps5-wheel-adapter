# Evidence: references, dependencies and what has actually been verified

Three evidence levels are kept separate throughout this project:

* **Automated** – host unit tests and builds on the development computer.
* **Author-reported** – what a reference project's author says worked on their hardware.
* **Physical (this rig)** – observed on your adapter, Hori, rig and PS5. Recorded in
  docs/TEST_PLAN.md as the tests are run. *Nothing is in this category yet.*

## References (inspected 2026-11-18)

| Project | Inspected | Author-reported hardware result | Not demonstrated |
| --- | --- | --- | --- |
| jfedor2/wheel-adapter | `87e03d1`, tag r2026-11-07 | PS5 + GT7 with Driving Force Pro; auth with MagicBoots PS4 v1.1, PXN N5, Razer Raion; FFB works; no 8-minute timeout "with the right controller or dongle". README documents the same two-port Pico wiring as this adapter (GP0/GP1, GP2/GP3, VBUS pin 40, GND pin 38). DualShock 4 explicitly unsupported. | Simagic/VRS, a 4-device hub behind one port, Hori 0F0D:0087. The two-port build is 12 days old; the README does not say which auth controllers were tried with it. |
| bilgon32/ps5-wheel-adapter | `9e2d4c8`, tested build = release firmware-v2026.10.20 | PS5 + GT7 with native G27, clutch, shifter, Arduino load-cell brake, FFB; **Hori Wired Controller Light for PS4** authenticates. Built from pico-sdk 1.5.1 `6a7db34`, jfedor2/tinyusb `8ef98ab`, Pico-PIO-USB `d00a10a`, Arm GCC 15.2.Rel1, **one** host port with a hub. | This rig; whether the Wired Controller Light is the same model/revision as 0F0D:0087; two host ports. No project-wide licence (code not imported). |
| sekigon-gonnoc/Pico-PIO-USB | jfedor2 fork `fc89362` (= upstream 0.7.3 + 9 commits) | USB host/device in PIO, hub and multi-port support. Discussion 72: a user (not the maintainer) states extra ports are added with `pio_usb_host_add_port()` after `tuh_init()` and share the same PIO state machines. | PS5 authentication; any wheel adapter. |
| hathach/tinyusb | jfedor2 fork `c7d880e` (= master 2026-10-20 + 6 commits) | Host/device stack. This revision has asynchronous enumeration and queues async control transfers when EP0 is busy. | PS5 authentication. |
| Pelsin/RP2040-DualSense-Mayflash-S5 | `295bd2c` | PS5 operation as a DualSense using a Mayflash S5 dongle and a vendor-provided secret. | Anything with a Hori or G29 identity; the dongle is not part of this design. |

## Why these dependency pins

The pins are exactly those of jfedor2 r2026-11-07, the only reference whose
**architecture matches** (two PIO-USB root ports on these pins). Relevant fork changes:

* TinyUSB `892789c` *usbh: make room for a deferred attach on every roothub port*:
  upstream sizes the deferred-attach queue by `CFG_TUH_HUB`; with two root ports and a
  hub attaching together at boot it overflowed, and the dropped attach left the port
  ignored until reboot. That is precisely the "everything connected at power-up" case.
* TinyUSB `5a1b04b` keeps a build-selected `SYS_CLK_HZ` (192 MHz) instead of forcing 120 MHz.
* Pico-PIO-USB fork: RX/ACK timing fixes, early-EOP handling, stuck-line RX overflow
  protection, and detection of a device leaving before its port reset.

bilgon32's tested set is older (pico-sdk 1.5.1, single port) and would need the
multi-root-port fix back-ported; it was not chosen. The compiler is Arm GNU Toolchain
14.2.Rel1 (official Arm binary, SHA-256 pinned); the references were built with GCC 10.3
(jfedor2 CI, Ubuntu 22.04) and 15.2.Rel1 (bilgon32). The compiler is not expected to change
USB behaviour, but it is recorded in dist/BUILDINFO.txt for every build.

## Stack behaviour verified by reading the pinned source

* **Default-address routing**: Pico-PIO-USB keys every endpoint by (root port, address,
  endpoint) (`_find_ep` in `pio_usb_host.c`), so address 0 on port A and port B are
  distinct; TinyUSB enumerates one device at a time (`enumerating_daddr`) and records the
  root port of address 0 (`hcd_setup_send(rhport, 0, …)`).
* **Shared address allocation**: TinyUSB assigns addresses from one table across root
  ports; completions are routed by address, which is unique.
* **Control-transfer ownership**: one EP0 slot for the whole host; an async
  `tuh_control_xfer` issued while it is busy is queued (`CFG_TUH_CONTROL_PENDING_QUEUE_SZ`
  = 4 with hubs) and returns false only if the queue is full. The adapter keeps at most
  one auth transfer outstanding and retries rejected submissions, so it cannot starve
  enumeration or the hub driver.
* **Non-blocking enumeration**: all enumeration delays use `usbh_defer_func_ms_async`, so
  the single loop keeps servicing the console while devices enumerate.
* **HID device control buffer**: SET/GET_REPORT use `epbuf->ctrl`, separate from the
  interrupt-OUT buffer. The 2023 workaround in the reference (F0 payload overwritten by
  FFB data) addressed a shared-buffer race that this revision no longer has.

## What the Hori VID/PID does and does not establish

0F0D:0087 only names a Hori product ID. Whether this unit implements the licensed signing
exchange is checked twice, independently of the console:

1. the firmware requires feature reports F0/F1/F2/F3 in its report descriptor
   (logged as `AUTH_DESC`), and
2. `tools/auth_probe.py --target hori` runs a full exchange from the computer and verifies
   the RSA-PSS signature against the public key the controller returns.

Passing both proves the controller signs. Only the PS5 can show that the console accepts
that controller's certificate for a G29 (the 8-minute timeout is the tell-tale; see
TEST_PLAN T6).
