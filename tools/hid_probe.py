#!/usr/bin/env python3
"""Read-only HID discovery on the development computer.

  hid_probe.py                       list HID interfaces of the rig, Hori and adapter
  hid_probe.py --all                 list every HID interface
  hid_probe.py --dump OUTDIR         save report descriptors of the listed interfaces
  hid_probe.py --watch VID:PID       print input reports and which bits change
                                     (e.g. 0483:0522 with the rig plugged into this computer)

Nothing is written to any device.
"""
import argparse
import os
import sys
import time

KNOWN = {
    (0x0F0D, 0x0087): "Hori (auth)",
    (0x05E3, 0x0610): "rig hub",
    (0x0483, 0x0522): "Simagic wheelbase/GT Neo",
    (0x0483, 0xA3BE): "VRS DFP pedals",
    (0x0FD9, 0x0063): "Stream Deck Mini",
    (0x8087, 0x1024): "additional HID (unverified)",
    (0x046D, 0xC260): "this adapter (G29 identity)",
}


def interfaces(show_all):
    import hid
    out = []
    for d in hid.enumerate():
        key = (d["vendor_id"], d["product_id"])
        if show_all or key in KNOWN:
            out.append(d)
    return out


def read_descriptor(path):
    import hid
    dev = hid.device()
    dev.open_path(path)
    try:
        return bytes(dev.get_report_descriptor())
    finally:
        dev.close()


def list_cmd(show_all, dump_dir):
    found = interfaces(show_all)
    if not found:
        print("none of the expected devices is connected to this computer")
    for d in found:
        key = (d["vendor_id"], d["product_id"])
        print("%04X:%04X %-28s itf=%-2d usage=%04X:%04X %r %r" % (
            key[0], key[1], KNOWN.get(key, ""), d["interface_number"], d["usage_page"], d["usage"],
            d["manufacturer_string"], d["product_string"]))
        try:
            desc = read_descriptor(d["path"])
            print("    report descriptor %d bytes" % len(desc))
            if dump_dir:
                os.makedirs(dump_dir, exist_ok=True)
                name = "%04x_%04x_itf%d_%04x_%04x.bin" % (key[0], key[1], d["interface_number"], d["usage_page"], d["usage"])
                with open(os.path.join(dump_dir, name), "wb") as f:
                    f.write(desc)
                print("    saved %s" % os.path.join(dump_dir, name))
        except (OSError, IOError) as ex:
            print("    descriptor not readable: %s" % ex)
    return 0


def watch_cmd(spec, seconds):
    import hid
    vid, pid = (int(x, 16) for x in spec.split(":"))
    devs = hid.enumerate(vid, pid)
    if not devs:
        print("%s not connected" % spec)
        return 1
    dev = hid.device()
    dev.open_path(devs[0]["path"])
    dev.set_nonblocking(True)
    prev = None
    end = time.time() + seconds if seconds else None
    print("reading %s; press controls one at a time (Ctrl-C to stop)" % spec)
    try:
        while end is None or time.time() < end:
            r = dev.read(64)
            if not r:
                time.sleep(0.002)
                continue
            r = bytes(r)
            if prev is not None and len(prev) == len(r) and r != prev:
                changed = [i * 8 + b for i in range(len(r)) for b in range(8) if (r[i] ^ prev[i]) >> b & 1]
                print("len %d id %d changed packet bits %s  bytes %s" % (
                    len(r), r[0], changed[:16], " ".join("%d:%02x" % (i, r[i]) for i in sorted({c // 8 for c in changed}))))
            prev = r
    except KeyboardInterrupt:
        pass
    dev.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--dump", metavar="OUTDIR")
    ap.add_argument("--watch", metavar="VID:PID")
    ap.add_argument("--seconds", type=float, default=0)
    args = ap.parse_args(argv)
    if args.watch:
        return watch_cmd(args.watch, args.seconds)
    return list_cmd(args.all, args.dump)


if __name__ == "__main__":
    sys.exit(main())
