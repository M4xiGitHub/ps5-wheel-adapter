# Third-party notices

## jfedor2/wheel-adapter (MIT)

`firmware/src/g29.c` (HID report descriptor, feature 0x03 and 0xF3 replies) and the
device/configuration descriptor values in `firmware/src/usb_descriptors.c` are copied
from jfedor2/wheel-adapter r2026-11-07. The authentication sequence in
`firmware/src/auth.c` follows the same protocol; the code is new.

```
Copyright (c) 2026 Jacek Fedorynski

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Build dependencies (fetched by scripts/setup.sh, not redistributed here)

| Dependency | Licence (see the file in `deps/<name>/`) |
| --- | --- |
| Raspberry Pi Pico SDK 2.3.1 | BSD-3-Clause (`LICENSE.TXT`) |
| TinyUSB (jfedor2 fork) | MIT (`LICENSE`) |
| Pico-PIO-USB (jfedor2 fork) | MIT (`LICENSE`) |
| picotool 2.3.1 | BSD-3-Clause (`LICENSE.TXT`) |
| Arm GNU Toolchain 14.2.Rel1 | GPL/LGPL with runtime exceptions; newlib licences |

The UF2 images link the Pico SDK, TinyUSB and Pico-PIO-USB; keep their notices with any
binaries you pass on.

Code from bilgon32/ps5-wheel-adapter (no project-wide licence) and from
Pelsin/RP2040-DualSense-Mayflash-S5 was read for reference only and not copied.
