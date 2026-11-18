#!/usr/bin/env python3
"""Decode the adapter's diagnostic records.

Sources:
  * a flash dump read in BOOTSEL mode (tools/pull_log.sh), which contains one slot
    per boot of the adapter, so a PS5 session survives unplugging the adapter;
  * live "L <hex>" lines from the bench build (tools/adapter_monitor.py).

Usage:
  adapter_log.py DUMP.bin               list the boots stored in the dump
  adapter_log.py DUMP.bin --boot N      decode one boot (default: newest console-build boot)
  adapter_log.py DUMP.bin --all         decode every boot
  adapter_log.py DUMP.bin --boot N --descriptors OUTDIR   also save captured HID report descriptors

Record layout and type numbers mirror firmware/src/diag.h.
"""
import argparse
import binascii
import os
import struct
import sys

REGION_OFFSET = 0x100000
SLOT_SIZE = 0x40000
SLOTS = 4
PAGE = 256
SLOT_MAGIC = 0x4C354750
HEADER_FMT = "<IHHIIBBHII"  # magic version rec_size boot fw variant reset clk slot_size crc
HEADER_SIZE = struct.calcsize(HEADER_FMT)

REC_TYPES = {
    1: "BOOT", 2: "PORT_EVENT", 3: "DEV_MOUNT", 4: "DEV_ROLE", 5: "DEV_UMOUNT", 6: "HID_MOUNT",
    7: "HID_DESC_CHUNK", 8: "LAYOUT", 9: "LAYOUT_FIELD", 10: "AUTH_DESC", 11: "AUTH", 12: "FIRST_INPUT",
    13: "INPUT_REJECT", 14: "USB_DEV", 15: "CONSOLE_REQ", 16: "CONSOLE_OUTPUT", 17: "PS_BUTTON",
    18: "SRC_BUTTON", 19: "HEARTBEAT", 20: "HEARTBEAT2", 21: "HEARTBEAT3", 22: "ERROR", 23: "LOG_FULL",
    24: "FFB", 25: "HEARTBEAT4",
}
FFB_EVENTS = {1: "FORMAT", 2: "FIRST_CMD", 3: "UNSUPPORTED", 4: "EXTENDED", 5: "ACTIVE", 6: "RESET", 7: "STALE",
              8: "OUT_MODE", 100: "BENCH_TEST"}
FFB_RESETS = {1: "boot/new wheelbase", 2: "console gone", 3: "suspend", 4: "wheelbase gone", 5: "stale", 6: "test"}
LG_TYPES = {0x00: "constant", 0x01: "spring", 0x02: "damper", 0x03: "auto-centre spring", 0x04: "saw up",
            0x05: "saw down", 0x06: "trapezoid", 0x07: "rectangle", 0x08: "variable", 0x09: "ramp",
            0x0A: "square", 0x0B: "hi-res spring", 0x0C: "hi-res damper", 0x0D: "hi-res auto-centre",
            0x0E: "friction"}
TYPE = {v: k for k, v in REC_TYPES.items()}

ROLES = {0: "none", 1: "unknown", 2: "hub", 3: "auth", 4: "wheel", 5: "pedals", 6: "passive"}
AUTH_EVENTS = {
    1: "CONSOLE_F3", 2: "CONSOLE_NONCE_PAGE", 3: "CONSOLE_NONCE_BAD", 4: "JOB_START", 5: "JOB_CANCELLED",
    6: "DS_STEP_OK", 7: "DS_STEP_FAIL", 8: "DS_SUBMIT_REJECTED", 9: "DS_SIGNED", 10: "DS_READY",
    11: "DS_PAGE_CHECK", 12: "DS_FAILED", 13: "DS_XFER_TIMEOUT", 14: "DS_STALE_COMPLETE", 15: "CONSOLE_F2",
    16: "CONSOLE_F1_PAGE", 17: "DELIVERED", 18: "DEVICE_PRESENT",
}
AUTH_STATES = {0: "idle", 1: "wait_device", 2: "reset", 3: "nonce", 4: "poll", 5: "sig", 6: "ready", 7: "failed"}
LAYOUT_STATUS = {0: "matches documented layout", 1: "DIFFERS from documented layout (descriptor used)", 2: "UNUSABLE"}
FIELDS = {1: "steer(X)", 2: "hat", 3: "button1", 4: "pedal X (clutch src)", 5: "pedal Y (brake)", 6: "pedal Z (throttle)"}
USB_DEV_EVENTS = {1: "CONFIGURED by host (SET_CONFIGURATION)", 2: "unconfigured", 3: "SUSPENDED", 4: "resumed"}
REPORT_TYPES = {0: "invalid", 1: "input", 2: "output", 3: "feature"}
PORT_NAMES = {1: "A (GP0/GP1)", 2: "B (GP2/GP3)"}
ERRORS = {1: "flash log unavailable", 2: "pio_usb_host_add_port failed"}
HB_FLAGS = [(0x01, "console-configured"), (0x02, "auth-device-ok"), (0x04, "wheel-input"), (0x08, "pedal-input"),
            (0x10, "portA-attached"), (0x20, "portB-attached"), (0x40, "suspended")]


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def s16(b, o):
    v = u16(b, o)
    return v - 0x10000 if v & 0x8000 else v


class Record:
    __slots__ = ("t_ms", "seq", "type", "a", "d")

    def __init__(self, raw):
        self.t_ms, self.seq, self.type, self.a = struct.unpack_from("<IHBB", raw, 0)
        self.d = bytes(raw[8:16])

    @property
    def name(self):
        return REC_TYPES.get(self.type, "type%d" % self.type)


def describe(r):
    d, a = r.d, r.a
    n = r.name
    if n == "BOOT":
        fw = u16(d, 6)
        return "boot #%d build=%s reset=0x%02x clk=%dMHz fw=%d.%d.%d" % (
            u32(d, 0), "bench" if a else "console", d[4], d[5], 0, fw >> 8, fw & 0xFF)
    if n == "PORT_EVENT":
        return "port %s %s (count %d)" % (PORT_NAMES.get(a, a), "ATTACH" if d[0] else "REMOVE", u32(d, 2))
    if n == "DEV_MOUNT":
        return "dev %d %04X:%04X rhport=%d (%s) hub_addr=%d hub_port=%d speed=%s" % (
            a, u16(d, 4), u16(d, 6), d[0], PORT_NAMES.get(d[0], "?"), d[1], d[2],
            {0: "FS", 1: "LS", 2: "HS"}.get(d[3], d[3]))
    if n == "DEV_ROLE":
        return "dev %d role=%s topology=%s (expected port %s, behind hub=%d)" % (
            a, ROLES.get(d[0], d[0]), "OK" if d[1] else "UNEXPECTED", PORT_NAMES.get(d[2], "-"), d[3])
    if n == "DEV_UMOUNT":
        return "dev %d removed (role %s)" % (a, ROLES.get(d[0], d[0]))
    if n == "HID_MOUNT":
        return "dev %d HID idx=%d itf=%d report-descriptor %d bytes crc32=%08x" % (a, d[0], d[1], u16(d, 2), u32(d, 4))
    if n == "HID_DESC_CHUNK":
        return "dev %d idx=%d desc[%d..] %s" % (a, d[0], d[1] * 6, d[2:8].hex())
    if n == "LAYOUT":
        what = ROLES.get(a, a)
        return "%s layout: %s mismatch=0x%02x report_id=%d payload=%d bits buttons=%d hat=%d itf_idx=%d" % (
            what, LAYOUT_STATUS.get(d[0], d[0]), d[1], d[2], u16(d, 3), d[5], d[6], d[7])
    if n == "LAYOUT_FIELD":
        return "  field %s: bit %d size %d report %d logical %d..%d" % (
            FIELDS.get(a, a), u16(d, 0), d[2], d[3], s16(d, 4), u16(d, 6))
    if n == "AUTH_DESC":
        ok = d[0] & 1
        return "auth candidate dev %d idx=%d: %s (F0 %d bits, F2 %d bits, F3 %d bits)" % (
            a, d[1], "VALID auth interface" if ok else "NO auth reports - not used", u16(d, 2), u16(d, 4), u16(d, 6))
    if n == "AUTH":
        ev = AUTH_EVENTS.get(a, "ev%d" % a)
        arg, val, st = d[0], u32(d, 1), AUTH_STATES.get(d[5], d[5])
        extra = ""
        if ev == "CONSOLE_NONCE_PAGE":
            extra = "page %d nonce_id=0x%02x crc=%s" % (arg, val & 0xFF, "ok" if (val >> 8) & 1 else "MISMATCH")
        elif ev == "CONSOLE_NONCE_BAD":
            extra = "reason=%s page=%d nonce_id=0x%02x len=%d" % (
                {1: "length", 2: "page range", 3: "sequence"}.get(arg, arg), val & 0xFF, (val >> 8) & 0xFF, val >> 16)
        elif ev == "JOB_START":
            extra = "nonce_id=0x%02x job #%d" % (arg, val)
        elif ev == "DS_STEP_OK":
            extra = "report 0x%02x page %d" % (arg, val & 0xFF)
        elif ev == "DS_STEP_FAIL":
            extra = "report 0x%02x retry %d len %d" % (arg, val & 0xFF, val >> 8)
        elif ev == "DS_SIGNED":
            extra = "Hori signed after %d ms, %d status polls" % (val, arg)
        elif ev == "DS_READY":
            extra = "19 signature pages buffered, %d ms after nonce" % val
        elif ev == "DS_PAGE_CHECK":
            extra = "page %d crc=%s nonce_id=%s page_field=%s(%d)" % (
                arg, "ok" if val & 1 else "MISMATCH", "ok" if val & 2 else "MISMATCH", "ok" if val & 4 else "MISMATCH",
                (val >> 8) & 0xFF)
        elif ev == "DS_FAILED":
            extra = "last report 0x%02x reason=%s" % (arg, {0: "retries exhausted", 1: "signing timeout",
                                                             2: "transfer backstop timeout"}.get(val, val))
        elif ev == "CONSOLE_F2":
            extra = "console told %s (query #%d)" % ("READY" if arg == 0 else "busy", val)
        elif ev == "CONSOLE_F1_PAGE":
            extra = "served page %d%s" % (arg, "" if val else " BEFORE signature ready")
        elif ev == "DELIVERED":
            extra = "complete signature delivered to console (nonce 0x%02x, total %d)" % (arg, val)
        elif ev == "DEVICE_PRESENT":
            extra = "auth controller %s" % ("available" if arg else "gone")
        elif ev in ("DS_SUBMIT_REJECTED", "DS_XFER_TIMEOUT", "DS_STALE_COMPLETE", "JOB_CANCELLED"):
            extra = "arg=0x%02x value=%d" % (arg, val)
        return "auth %-18s %s [state %s]" % (ev, extra, st)
    if n == "FIRST_INPUT":
        return "first %s report: len %d bytes %s" % (ROLES.get(a, a), u16(d, 0), d[2:8].hex())
    if n == "INPUT_REJECT":
        return "%s report rejected: len %d first byte 0x%02x (total %d)" % (ROLES.get(a, a), u16(d, 0), d[2], u32(d, 4))
    if n == "USB_DEV":
        return "upstream %s (bus resets so far %d)" % (USB_DEV_EVENTS.get(a, a), u32(d, 0))
    if n == "CONSOLE_REQ":
        return "host first %s report 0x%02x type=%s len=%d" % (
            "SET" if d[1] else "GET", a, REPORT_TYPES.get(d[0], d[0]), u16(d, 2))
    if n == "CONSOLE_OUTPUT":
        return "host first output report (FFB/LED data): id=%d data %s" % (a, d.hex())
    if n == "PS_BUTTON":
        return "PS button %s (source button %d)" % ("PRESSED" if a else "released", d[0])
    if n == "SRC_BUTTON":
        return "source button %d %s" % (a, "down" if d[0] else "up")
    if n == "HEARTBEAT":
        flags = ",".join(name for bit, name in HB_FLAGS if a & bit) or "-"
        return "heartbeat up=%ds sent=%d wheel_rx=%d pedal_rx=%d [%s]" % (u16(d, 0), u16(d, 2), u16(d, 4), u16(d, 6), flags)
    if n == "HEARTBEAT2":
        return "heartbeat auth=%s ok=%d delivered=%d failed=%d console_out=%d" % (
            AUTH_STATES.get(a, a), u16(d, 0), u16(d, 2), u16(d, 4), u16(d, 6))
    if n == "HEARTBEAT3":
        return "heartbeat flash_dropped=%d live_dropped=%d send_busy=%d input_rejects=%d" % (
            u16(d, 0), u16(d, 2), u16(d, 4), u16(d, 6))
    if n == "ERROR":
        return "ERROR %s detail %s" % (ERRORS.get(a, a), d.hex())
    if n == "LOG_FULL":
        return "flash slot full - later records were not stored"
    if n == "FFB":
        ev = FFB_EVENTS.get(a, "ev%d" % a)
        arg, val = d[0], u32(d, 1)
        if ev == "FORMAT":
            return "ffb wheelbase output report %s: id %d, %d bytes, %d byte values" % (
                "OK" if arg else "UNUSABLE - FFB off", val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFFFF)
        if ev == "FIRST_CMD":
            return "ffb first console command %02x %02x %02x %02x %02x (op %d, type %s)" % (
                arg, val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, val >> 24, arg & 0x0F,
                LG_TYPES.get(val & 0xFF, "0x%02x" % (val & 0xFF)) if (arg & 0x0F) in (0, 1, 12) else "-")
        if ev == "UNSUPPORTED":
            return "ffb console uses %s effects (not translated), count %d" % (LG_TYPES.get(arg, "0x%02x" % arg), val)
        if ev == "EXTENDED":
            return "ffb extended/other command (byte1 0x%02x) ignored, total %d" % (arg, val)
        if ev == "ACTIVE":
            m = val & 0xFFFF
            m = m - 0x10000 if m & 0x8000 else m
            return "ffb constant force %s (magnitude %d / 10000)" % ("STARTED" if arg else "stopped", m)
        if ev == "RESET":
            return "ffb reset: %s" % FFB_RESETS.get(arg, arg)
        if ev == "STALE":
            return "ffb STALE: no console command for %d ms with force held - stopped" % val
        if ev == "OUT_MODE":
            return "ffb wheelbase has no interrupt OUT endpoint: using control SET_REPORT"
        if ev == "BENCH_TEST":
            m = u16(d, 0)
            return "ffb bench test raw magnitude %d" % (m - 0x10000 if m & 0x8000 else m)
        return "ffb %s arg=%d value=%d" % (ev, arg, val)
    if n == "HEARTBEAT4":
        return "heartbeat ffb %s%s%s console_cmds=%d packets=%d unsupported=%d max_gap=%dms" % (
            "running" if a & 1 else "idle", "" if a & 2 else " NO-FORMAT", " control-mode" if a & 4 else "",
            u16(d, 0), u16(d, 2), u16(d, 4), u16(d, 6))
    return "a=%d d=%s" % (a, d.hex())


def format_record(r):
    return "%9.3f  #%-5d %-15s %s" % (r.t_ms / 1000.0, r.seq, r.name, describe(r))


# ---------------------------------------------------------------- flash dump

def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def parse_slot(blob, slot):
    base = slot * SLOT_SIZE
    raw = blob[base:base + HEADER_SIZE]
    if len(raw) < HEADER_SIZE:
        return None
    f = struct.unpack(HEADER_FMT, raw)
    hdr = dict(zip(("magic", "version", "rec_size", "boot", "fw", "variant", "reset", "clk", "slot_size", "crc"), f))
    if hdr["magic"] != SLOT_MAGIC or hdr["crc"] != crc32(raw[:-4]):
        return None
    recs = []
    for off in range(base + PAGE, base + SLOT_SIZE, 16):
        chunk = blob[off:off + 16]
        if len(chunk) < 16 or chunk == b"\xff" * 16:
            continue  # unwritten (end of log or a partial page tail)
        recs.append(Record(chunk))
    recs.sort(key=lambda r: (r.t_ms, r.seq))
    hdr["slot"] = slot
    hdr["records"] = recs
    return hdr


def parse_dump(blob):
    """blob starts at flash offset REGION_OFFSET (picotool save -r 0x10100000 0x10200000)."""
    boots = [s for s in (parse_slot(blob, i) for i in range(SLOTS)) if s]
    boots.sort(key=lambda s: s["boot"])
    return boots


def descriptors(recs):
    """Reassemble HID report descriptors from HID_MOUNT + HID_DESC_CHUNK records."""
    out = {}
    lens = {}
    for r in recs:
        if r.name == "HID_MOUNT":
            lens[(r.a, r.d[0])] = (u16(r.d, 2), u32(r.d, 4))
        elif r.name == "HID_DESC_CHUNK":
            out.setdefault((r.a, r.d[0]), {})[r.d[1]] = r.d[2:8]
    result = {}
    for key, chunks in out.items():
        n, crc = lens.get(key, (0, None))
        data = b"".join(chunks.get(i, b"\x00" * 6) for i in range(max(chunks) + 1))[:n]
        result[key] = (data, crc is not None and crc32(data) == crc)
    return result


def summarize(recs):
    s = {
        "duration_s": (recs[-1].t_ms - recs[0].t_ms) / 1000.0 if recs else 0,
        "configured": any(r.name == "USB_DEV" and r.a == 1 for r in recs),
        "console_auth_requests": sum(1 for r in recs if r.name == "AUTH" and r.a == 1),
        "jobs_ok": sum(1 for r in recs if r.name == "AUTH" and AUTH_EVENTS.get(r.a) == "DS_READY"),
        "jobs_failed": sum(1 for r in recs if r.name == "AUTH" and AUTH_EVENTS.get(r.a) == "DS_FAILED"),
        "delivered": sum(1 for r in recs if r.name == "AUTH" and AUTH_EVENTS.get(r.a) == "DELIVERED"),
        "ps_presses": sum(1 for r in recs if r.name == "PS_BUTTON" and r.a == 1),
        "wheel_input": any(r.name == "FIRST_INPUT" and r.a == 4 for r in recs),
        "pedal_input": any(r.name == "FIRST_INPUT" and r.a == 5 for r in recs),
        "auth_device_valid": any(r.name == "AUTH_DESC" and r.d[0] & 1 for r in recs),
        "log_full": any(r.name == "LOG_FULL" for r in recs),
    }
    s["host_looked_like_console"] = s["console_auth_requests"] > 0
    return s


def print_boot(b, desc_dir=None):
    recs = b["records"]
    print("=== boot #%d (slot %d) build=%s fw=0x%06x clk=%dMHz reset=0x%02x, %d records" % (
        b["boot"], b["slot"], "bench" if b["variant"] else "console", b["fw"], b["clk"], b["reset"], len(recs)))
    for r in recs:
        if r.name != "HID_DESC_CHUNK":
            print(format_record(r))
    s = summarize(recs)
    print("--- summary")
    for k, v in s.items():
        print("  %-26s %s" % (k, v))
    if not s["host_looked_like_console"]:
        print("  NOTE: no console auth request (GET F3) in this boot: this was not a PS5 session.")
    if desc_dir:
        os.makedirs(desc_dir, exist_ok=True)
        for (daddr, idx), (data, ok) in sorted(descriptors(recs).items()):
            path = os.path.join(desc_dir, "boot%d_dev%d_idx%d.bin" % (b["boot"], daddr, idx))
            with open(path, "wb") as f:
                f.write(data)
            print("  saved %s (%d bytes, crc %s)" % (path, len(data), "ok" if ok else "MISMATCH"))


UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
XIP_BASE = 0x10000000


def region_from_uf2(data):
    """Extracts the log region from a UF2 image (e.g. CURRENT.UF2 copied from the RPI-RP2
    drive in BOOTSEL mode). Returns (blob, bytes_covered)."""
    start = XIP_BASE + REGION_OFFSET
    size = SLOTS * SLOT_SIZE
    blob = bytearray(b"\xff" * size)
    covered = 0
    for off in range(0, len(data) - 511, 512):
        m0, m1, _flags, addr, n = struct.unpack_from("<IIIII", data, off)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or struct.unpack_from("<I", data, off + 508)[0] != UF2_MAGIC_END:
            continue
        if n > 476 or addr + n <= start or addr >= start + size:
            continue
        lo, hi = max(addr, start), min(addr + n, start + size)
        blob[lo - start:hi - start] = data[off + 32 + (lo - addr):off + 32 + (hi - addr)]
        covered += hi - lo
    return bytes(blob), covered


def load_region(path):
    """Accepts a raw picotool dump of the log region or a UF2 image of the flash."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) >= 512 and struct.unpack_from("<II", data, 0) == (UF2_MAGIC0, UF2_MAGIC1):
        blob, covered = region_from_uf2(data)
        if covered == 0:
            print("%s is a UF2 image without the log region (0x%08X..). Read the log with picotool instead:"
                  % (path, XIP_BASE + REGION_OFFSET))
            print("  picotool save -r 0x10100000 0x10200000 flashlog.bin")
            return None
        return blob
    return data


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="picotool dump of 0x10100000-0x10200000, or CURRENT.UF2 from the RPI-RP2 drive")
    ap.add_argument("--boot", type=int)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--descriptors", metavar="OUTDIR")
    args = ap.parse_args(argv)
    blob = load_region(args.dump)
    if blob is None:
        return 1
    boots = parse_dump(blob)
    if not boots:
        print("no valid log slots in %s" % args.dump)
        return 1
    print("boots in dump:")
    for b in boots:
        s = summarize(b["records"])
        print("  #%-4d slot %d %-7s %5d records %7.1fs  console-auth=%s delivered=%d PS=%d" % (
            b["boot"], b["slot"], "bench" if b["variant"] else "console", len(b["records"]), s["duration_s"],
            "yes" if s["host_looked_like_console"] else "no", s["delivered"], s["ps_presses"]))
    if args.all:
        sel = boots
    elif args.boot is not None:
        sel = [b for b in boots if b["boot"] == args.boot]
    else:
        console = [b for b in boots if not b["variant"]]
        sel = [console[-1]] if console else [boots[-1]]
    for b in sel:
        print()
        print_boot(b, args.descriptors)
    return 0


if __name__ == "__main__":
    sys.exit(main())
