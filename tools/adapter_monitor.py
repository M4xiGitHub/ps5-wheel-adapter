#!/usr/bin/env python3
"""Live diagnostics for the BENCH build (ps5-adapter-bench-*.uf2) over its CDC port.

  adapter_monitor.py                 decode events + a status line every second
  adapter_monitor.py --seconds 20    stop after 20 s (for scripted checks)
  adapter_monitor.py --wizard        record which wheel button should act as which G29 control
  adapter_monitor.py --axes          measure raw steering/pedal ranges and directions
  adapter_monitor.py --bootsel       reboot the adapter into the USB bootloader (for flashing)
  adapter_monitor.py --raw FILE      also append every received line to FILE

The console build has no CDC port; use tools/pull_log.sh for it.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adapter_log  # noqa: E402

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover
    serial = None

STATUS_FMT = "<IBBBB5i16s3sB4H6I"
STATUS_SIZE = struct.calcsize(STATUS_FMT)
STATUS_FIELDS = ("t_ms", "flags", "auth_state", "valid", "reserved", "steer", "hat", "throttle", "brake", "clutch",
                 "src_buttons", "g29_btn012", "g29_btn3", "g29_wheel", "g29_throttle", "g29_brake", "g29_clutch",
                 "reports_sent", "wheel_rx", "pedal_rx", "auth_ok", "auth_delivered", "auth_failed")

G29_BITS = [  # (byte index in g29_btn012/btn3, bit, name)
    (0, 4, "square"), (0, 5, "cross"), (0, 6, "circle"), (0, 7, "triangle"),
    (1, 0, "L1"), (1, 1, "R1"), (1, 2, "L2"), (1, 3, "R2"), (1, 4, "share"), (1, 5, "options"), (1, 6, "L3"),
    (1, 7, "R3"), (2, 0, "PS"), (2, 1, "touchpad"),
    (3, 0, "enter"), (3, 1, "dial_down"), (3, 2, "dial_up"), (3, 3, "minus"), (3, 4, "plus"),
]

WIZARD_TARGETS = [
    ("G29_BTN_CROSS", "Cross (confirm; needed for PS5 user selection)"),
    ("G29_BTN_CIRCLE", "Circle (back)"),
    ("G29_BTN_SQUARE", "Square"),
    ("G29_BTN_TRIANGLE", "Triangle"),
    ("G29_BTN_L1", "L1 (left paddle / shift down)"),
    ("G29_BTN_R1", "R1 (right paddle / shift up)"),
    ("G29_BTN_L2", "L2"),
    ("G29_BTN_R2", "R2"),
    ("G29_BTN_SHARE", "Share / Create"),
    ("G29_BTN_OPTIONS", "Options (pause menu)"),
    ("G29_BTN_L3", "L3"),
    ("G29_BTN_R3", "R3"),
    ("G29_BTN_PS", "PS button"),
    ("G29_BTN_PLUS", "G29 plus"),
    ("G29_BTN_MINUS", "G29 minus"),
    ("G29_BTN_DIAL_UP", "G29 dial clockwise"),
    ("G29_BTN_DIAL_DOWN", "G29 dial anticlockwise"),
    ("G29_BTN_ENTER", "G29 dial enter"),
    ("G29_BTN_DPAD_UP", "D-pad up (only if the wheel's hat does not already do it)"),
    ("G29_BTN_DPAD_RIGHT", "D-pad right (same)"),
    ("G29_BTN_DPAD_DOWN", "D-pad down (same)"),
    ("G29_BTN_DPAD_LEFT", "D-pad left (same)"),
]


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x046D and p.pid == 0xC260:
            return p.device
    return None


def parse_status(hexstr):
    raw = bytes.fromhex(hexstr)
    if len(raw) != STATUS_SIZE:
        return None
    return dict(zip(STATUS_FIELDS, struct.unpack(STATUS_FMT, raw)))


def pressed_source_buttons(st):
    out = []
    for i, byte in enumerate(st["src_buttons"]):
        for b in range(8):
            if byte >> b & 1:
                out.append(i * 8 + b + 1)
    return out


def g29_pressed(st):
    raw = st["g29_btn012"] + bytes([st["g29_btn3"]])
    return [name for (i, bit, name) in G29_BITS if raw[i] >> bit & 1]


def status_line(st):
    v = st["valid"]
    flags = ",".join(n for bit, n in adapter_log.HB_FLAGS if st["flags"] & bit) or "-"

    def val(mask, x):
        return str(x) if v & mask else "--"

    return ("[%7.1fs] %s | steer %s hat %s thr %s brk %s clu %s | src %s | G29 wheel %5d thr %5d brk %5d dpad %d %s | "
            "auth %s ok=%d delivered=%d failed=%d | sent=%d wheel_rx=%d pedal_rx=%d") % (
        st["t_ms"] / 1000.0, flags, val(1, st["steer"]), val(2, st["hat"]), val(4, st["throttle"]),
        val(8, st["brake"]), val(16, st["clutch"]), pressed_source_buttons(st) or "-", st["g29_wheel"],
        st["g29_throttle"], st["g29_brake"], st["g29_btn012"][0] & 0x0F, g29_pressed(st) or "",
        adapter_log.AUTH_STATES.get(st["auth_state"], st["auth_state"]), st["auth_ok"], st["auth_delivered"],
        st["auth_failed"], st["reports_sent"], st["wheel_rx"], st["pedal_rx"])


class Link:
    def __init__(self, port, raw_path=None):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.dtr = True
        self.buf = b""
        self.raw = open(raw_path, "a") if raw_path else None
        self.status = None

    def lines(self):
        data = self.ser.read(4096)
        if data:
            self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.decode("ascii", "replace").strip()
            if self.raw:
                self.raw.write(line + "\n")
            yield line

    def poll(self):
        """Returns decoded records; updates self.status."""
        recs = []
        for line in self.lines():
            if line.startswith("L ") and len(line) == 34:
                recs.append(adapter_log.Record(bytes.fromhex(line[2:])))
            elif line.startswith("S "):
                st = parse_status(line[2:])
                if st:
                    self.status = st
        return recs


def run_monitor(link, seconds):
    end = time.time() + seconds if seconds else None
    last = 0
    while end is None or time.time() < end:
        for r in link.poll():
            print(adapter_log.format_record(r), flush=True)
        if link.status and time.time() - last >= 1.0:
            last = time.time()
            print(status_line(link.status), flush=True)


def enter_pressed():
    """Non-blocking check for the Enter key (Windows console or POSIX terminal)."""
    if os.name == "nt":
        import msvcrt
        while msvcrt.kbhit():
            if msvcrt.getwch() in "\r\n":
                return True
        return False
    import select
    if select.select([sys.stdin], [], [], 0)[0]:
        sys.stdin.readline()
        return True
    return False


def wait_press(link, timeout):
    """Waits for a source button to go down then up. Returns its number, or None on Enter/timeout."""
    end = time.time() + timeout
    base = None
    pressed = None
    while time.time() < end:
        link.poll()
        if enter_pressed():
            return None
        st = link.status
        if not st:
            continue
        now = set(pressed_source_buttons(st))
        if base is None:
            base = now
            continue
        if pressed is None:
            new = sorted(now - base)
            if new:
                pressed = new[0]
                print("   source button %d down..." % pressed, end="", flush=True)
        elif pressed not in now:
            print(" released")
            return pressed
    return None


DIAG_TYPES = ("BOOT", "PORT_EVENT", "DEV_MOUNT", "DEV_ROLE", "HID_MOUNT", "LAYOUT", "LAYOUT_FIELD", "AUTH_DESC",
              "FIRST_INPUT", "INPUT_REJECT", "USB_DEV", "ERROR")


def explain_no_wheel(link, port, seen):
    """Prints why the wizard cannot start, from what the adapter itself reported."""
    print("\nNo wheel input arrived. What the adapter reported:")
    if link.status is None:
        print("  nothing: no status lines on %s." % port)
        print("  -> Is the BENCH firmware flashed (not the console one)? Unplug/replug the Pico and retry.")
        return
    print("  " + status_line(link.status))
    shown = [r for r in seen if r.name in DIAG_TYPES]
    for r in shown:
        print("  " + adapter_log.format_record(r))
    if not shown:
        print("  (boot records were already read by an earlier run: unplug and replug the Pico, then run")
        print("   'adapter_monitor.py --seconds 30' and send its output)")
    flags = link.status["flags"]
    if not flags & 0x20:
        print("  -> port B (GP2/GP3) shows no attached device: check the rig cable on port B.")
    elif not any(r.name == "LAYOUT" and r.a == 4 for r in seen):
        print("  -> no wheel layout record seen: look for DEV_MOUNT 0483:0522 above.")
    else:
        print("  -> check the 'wheel layout' line above: UNUSABLE means the Simagic report descriptor")
        print("     has no steering axis where the firmware looks for it.")
    print("Please send this whole output.")


def run_wizard(link, port):
    print("Button mapping wizard. For each control press and release the wheel button you want,")
    print("or press Enter to skip. Do not touch other buttons meanwhile.\n")
    print("Waiting up to 30 s for wheel input: turn the wheel a little or press any wheel button...")
    seen = []
    t0 = time.time()
    while not (link.status and link.status["flags"] & 0x04):  # HB_WHEEL_VALID: a wheel report arrived
        seen += link.poll()
        if time.time() - t0 > 30:
            explain_no_wheel(link, port, seen)
            return 1
    print("wheel input OK\n")
    entries = []
    used = {}
    for macro, text in WIZARD_TARGETS:
        print("-> %s" % text)
        b = wait_press(link, 60)
        if b is None:
            print("   skipped")
            continue
        if b in used:
            print("   note: source button %d is also mapped to %s" % (b, used[b]))
        used[b] = macro
        entries.append((b, macro))
    print("\nReplace CFG_BUTTON_MAP in firmware/config/adapter_config.h with:\n")
    print("#define CFG_BUTTON_MAP \\")
    for b, macro in entries:
        print("    { %d, %s }, \\" % (b, macro))
    print("")
    return 0


def run_axes(link, seconds):
    names = [("steer", 1), ("throttle", 4), ("brake", 8), ("clutch", 16)]
    lo, hi, first = {}, {}, {}
    print("For %d s: turn the wheel fully left and right, then press each pedal fully and release." % seconds)
    end = time.time() + seconds
    while time.time() < end:
        link.poll()
        st = link.status
        if not st:
            continue
        for n, m in names:
            if st["valid"] & m:
                v = st[n]
                first.setdefault(n, v)
                lo[n] = min(lo.get(n, v), v)
                hi[n] = max(hi.get(n, v), v)
    for n, _ in names:
        if n in first:
            print("%-8s first(rest)=%6d min=%6d max=%6d" % (n, first[n], lo[n], hi[n]))
        else:
            print("%-8s no data" % n)
    print("Compare with the logical ranges in the LAYOUT_FIELD records. For the pedals, 'rest' near min means"
          " CFG_PEDALS_RISE_WHEN_PRESSED=1 is right.")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--wizard", action="store_true")
    ap.add_argument("--axes", action="store_true")
    ap.add_argument("--bootsel", action="store_true")
    ap.add_argument("--raw")
    args = ap.parse_args(argv)
    if serial is None:
        print("pyserial missing: run scripts/setup.sh and use .venv/bin/python")
        return 2
    port = args.port or find_port()
    if not port:
        print("Bench build not found (no serial port with VID:PID 046D:C260). Is ps5-adapter-bench flashed and"
              " connected to this computer?")
        return 1
    link = Link(port, args.raw)
    if args.bootsel:
        link.ser.write(b"BOOTSEL\n")
        link.ser.flush()
        print("Sent BOOTSEL; the RPI-RP2 drive should appear.")
        return 0
    try:
        if args.wizard:
            return run_wizard(link, port)
        if args.axes:
            return run_axes(link, args.seconds or 20)
        run_monitor(link, args.seconds)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
