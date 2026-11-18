#!/usr/bin/env python3
"""Run one PS4-style signing exchange (F3 / F0 x5 / F2 / F1 x19) from this computer.

  auth_probe.py --target hori      Hori 0F0D:0087 plugged straight into this computer:
                                   proves whether this controller signs at all.
  auth_probe.py --target adapter   the adapter (046D:C260, console or bench build) with the
                                   Hori on port A: exercises the adapter's forwarding path
                                   exactly as the console drives it.

The response is split as signature(256) | serial(16) | N(256) | E(256) | CA signature(256)
and the nonce signature is checked against the embedded public key (RSA-PSS/SHA-256).
That proves a genuine signature over this nonce by the key the controller presents. It does
NOT prove the console accepts that key: only a PS5 test can show that.
"""
import argparse
import binascii
import os
import struct
import sys
import time

TARGETS = {"hori": (0x0F0D, 0x0087), "adapter": (0x046D, 0xC260)}
NONCE_LEN = 256
PAGE_DATA = 56


def crc32(b):
    return binascii.crc32(bytes(b)) & 0xFFFFFFFF


def nonce_pages(nonce, nonce_id):
    """Five 64-byte F0 reports (report ID first), each with CRC32 over ID + 59 bytes."""
    padded = nonce + bytes(5 * PAGE_DATA - len(nonce))
    out = []
    for n in range(5):
        body = bytes([0xF0, nonce_id, n, 0]) + padded[n * PAGE_DATA:(n + 1) * PAGE_DATA]
        out.append(body + struct.pack("<I", crc32(body)))
    return out


def check_page(page, report_id, nonce_id, n):
    """Returns (crc_ok, id_ok, page_ok) for a 64-byte F1 report."""
    crc_ok = len(page) == 64 and struct.unpack_from("<I", bytes(page), 60)[0] == crc32(page[:60])
    return crc_ok, page[1] == nonce_id, page[2] == n


def split_response(resp):
    return {
        "signature": resp[0:256], "serial": resp[256:272], "n": resp[272:528], "e": resp[528:784],
        "ca_signature": resp[784:1040],
    }


def verify(nonce, parts):
    """Tries RSA-PSS/SHA-256 with the embedded key. Returns a description of what verified."""
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding, rsa
    except ImportError:
        return None, "cryptography not installed"
    n = int.from_bytes(parts["n"], "big")
    e = int.from_bytes(parts["e"], "big")
    if n == 0 or e == 0:
        return False, "public key fields are zero"
    try:
        key = rsa.RSAPublicNumbers(e, n).public_key()
    except ValueError as ex:
        return False, "public key invalid: %s" % ex
    for salt, label in ((32, "salt 32"), (padding.PSS.AUTO, "salt auto")):
        try:
            key.verify(bytes(parts["signature"]), bytes(nonce),
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=salt), hashes.SHA256())
            return True, "RSA-PSS/SHA-256 (%s), key %d bits, e=%d" % (label, n.bit_length(), e)
        except InvalidSignature:
            continue
    return False, "signature does not verify with the embedded key (key %d bits, e=%d)" % (n.bit_length(), e)


def open_target(vid, pid):
    import hid
    cands = [d for d in hid.enumerate(vid, pid)]
    if not cands:
        return None, None
    # Windows opens one handle per top-level collection; the auth reports live in the
    # vendor collection (usage page 0xFFF0 on the G29). Try that first, then any
    # collection whose report descriptor declares feature report 0xF0.
    cands.sort(key=lambda d: 0 if d.get("usage_page") == 0xFFF0 else 1)
    for info in cands:
        if info.get("usage_page") == 0xFFF0:
            dev = hid.device()
            try:
                dev.open_path(info["path"])
                return dev, info
            except (OSError, IOError):
                pass
    for info in cands:
        dev = hid.device()
        try:
            dev.open_path(info["path"])
        except (OSError, IOError):
            continue
        try:
            desc = bytes(dev.get_report_descriptor())
        except (OSError, IOError, AttributeError):
            desc = b""
        if b"\x85\xf0" in desc or not desc:
            return dev, info
        dev.close()
    return None, cands[0]


def run(target, timeout_s, verbose):
    vid, pid = TARGETS[target]
    dev, info = open_target(vid, pid)
    if dev is None:
        print("%04X:%04X not found or not openable (macOS may ask for Input Monitoring permission)." % (vid, pid))
        return 1
    print("opened %04X:%04X %r usage_page=0x%04X" % (vid, pid, info.get("product_string"), info.get("usage_page", 0)))
    nonce = os.urandom(NONCE_LEN)
    nonce_id = os.urandom(1)[0] or 1
    t0 = time.time()

    r = dev.get_feature_report(0xF3, 8)
    print("GET F3 -> %s" % bytes(r).hex())
    for i, page in enumerate(nonce_pages(nonce, nonce_id)):
        n = dev.send_feature_report(page)
        if verbose:
            print("SET F0 page %d -> %d" % (i, n))
        if n < 0:
            print("SET F0 page %d failed" % i)
            return 1
    t_sent = time.time()
    polls = 0
    while True:
        r = bytes(dev.get_feature_report(0xF2, 16))
        polls += 1
        if len(r) >= 3 and r[2] == 0:
            break
        if time.time() - t_sent > timeout_s:
            print("no signature after %.1f s (%d polls, last F2 %s)" % (timeout_s, polls, r.hex()))
            return 1
        time.sleep(0.01)
    t_signed = time.time()
    print("F2 ready after %.0f ms, %d polls: %s" % ((t_signed - t_sent) * 1000, polls, r.hex()))
    f2_crc_ok = len(r) >= 16 and struct.unpack_from("<I", r, 12)[0] == crc32(r[:12])

    resp = b""
    bad = []
    for n in range(19):
        page = bytes(dev.get_feature_report(0xF1, 64))
        crc_ok, id_ok, page_ok = check_page(page, 0xF1, nonce_id, n)
        if not (crc_ok and id_ok and page_ok):
            bad.append((n, crc_ok, id_ok, page_ok, page[:4].hex()))
        resp += page[4:60]
    dt = time.time() - t0
    print("received %d response bytes in %.2f s total" % (len(resp), dt))
    print("F2 CRC32 %s; F1 pages with CRC/id/page problems: %s" % ("ok" if f2_crc_ok else "not standard", bad or "none"))
    parts = split_response(resp)
    print("serial: %s" % parts["serial"].hex())
    ok, how = verify(nonce, parts)
    print("nonce signature: %s - %s" % ({True: "VALID", False: "NOT VALID", None: "not checked"}[ok], how))
    dev.close()
    return 0 if ok else 3


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", choices=sorted(TARGETS), required=True)
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    return run(args.target, args.timeout, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
