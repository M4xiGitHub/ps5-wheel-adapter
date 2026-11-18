"""Tests for the host tools: flash-dump decoding and auth page helpers."""
import binascii
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import adapter_log  # noqa: E402
import auth_probe  # noqa: E402


def rec(t, seq, typ, a, d=b"\x00" * 8):
    return struct.pack("<IHBB", t, seq, typ, a) + d


def slot(boot, variant, records):
    hdr = struct.pack("<IHHIIBBHI", adapter_log.SLOT_MAGIC, 1, 16, boot, 0x000100, variant, 0x02, 192,
                      adapter_log.SLOT_SIZE)
    hdr += struct.pack("<I", binascii.crc32(hdr) & 0xFFFFFFFF)
    page0 = hdr + b"\xff" * (adapter_log.PAGE - len(hdr))
    body = b"".join(records)
    return (page0 + body).ljust(adapter_log.SLOT_SIZE, b"\xff")


class DumpDecoding(unittest.TestCase):
    def setUp(self):
        T = adapter_log.TYPE
        console = [
            rec(10, 0, T["BOOT"], 0, struct.pack("<IBBH", 7, 2, 192, 0x0100)),
            rec(900, 1, T["USB_DEV"], 1),
            rec(1000, 2, T["AUTH"], 1),  # CONSOLE_F3
            rec(5000, 3, T["AUTH"], 10, bytes([0]) + struct.pack("<I", 4000) + bytes([6, 0, 0])),  # DS_READY
            rec(5100, 4, T["AUTH"], 17, bytes([0x42]) + struct.pack("<I", 1) + bytes([6, 0, 0])),  # DELIVERED
            rec(9000, 5, T["PS_BUTTON"], 1, bytes([6]) + b"\x00" * 7),
        ]
        desc = bytes(range(13))
        crc = binascii.crc32(desc) & 0xFFFFFFFF
        console.append(rec(950, 6, T["HID_MOUNT"], 3, bytes([0, 0]) + struct.pack("<HI", len(desc), crc)))
        for c in range(3):
            console.append(rec(951, 7 + c, T["HID_DESC_CHUNK"], 3, bytes([0, c]) + desc[c * 6:c * 6 + 6].ljust(6, b"\0")))
        bench = [rec(5, 0, T["BOOT"], 1, struct.pack("<IBBH", 8, 2, 192, 0x0100))]
        empty = b"\xff" * adapter_log.SLOT_SIZE
        # slot 0 = boot 8 (newer, bench on the PC), slot 3 = boot 7 (console session)
        self.blob = slot(8, 1, bench) + empty + empty + slot(7, 0, console)

    def test_boots_and_selection(self):
        boots = adapter_log.parse_dump(self.blob)
        self.assertEqual([b["boot"] for b in boots], [7, 8])
        console = boots[0]
        s = adapter_log.summarize(console["records"])
        self.assertTrue(s["host_looked_like_console"])
        self.assertTrue(s["configured"])
        self.assertEqual(s["delivered"], 1)
        self.assertEqual(s["jobs_ok"], 1)
        self.assertEqual(s["ps_presses"], 1)
        self.assertFalse(adapter_log.summarize(boots[1]["records"])["host_looked_like_console"])

    def test_descriptor_reassembly(self):
        boots = adapter_log.parse_dump(self.blob)
        d = adapter_log.descriptors(boots[0]["records"])
        data, ok = d[(3, 0)]
        self.assertEqual(data, bytes(range(13)))
        self.assertTrue(ok)

    def test_every_type_has_a_description(self):
        for typ in adapter_log.REC_TYPES:
            r = adapter_log.Record(rec(1, 1, typ, 1, bytes(range(8))))
            self.assertIsInstance(adapter_log.format_record(r), str)

    def test_uf2_image_input(self):
        # CURRENT.UF2-style image: 256-byte payloads, firmware blocks plus the log region.
        blocks = []
        def block(addr, payload):
            hdr = struct.pack("<IIIIIIII", adapter_log.UF2_MAGIC0, adapter_log.UF2_MAGIC1, 0x2000, addr, 256, 0, 0,
                              0xE48BFF56)
            return hdr + payload.ljust(476, b"\0") + struct.pack("<I", adapter_log.UF2_MAGIC_END)
        blocks.append(block(0x10000000, b"\x11" * 256))  # firmware, outside the region
        base = adapter_log.XIP_BASE + adapter_log.REGION_OFFSET
        for off in range(0, len(self.blob), 256):
            chunk = self.blob[off:off + 256]
            if chunk != b"\xff" * 256:
                blocks.append(block(base + off, chunk))
        blob, covered = adapter_log.region_from_uf2(b"".join(blocks))
        self.assertGreater(covered, 0)
        self.assertEqual([b["boot"] for b in adapter_log.parse_dump(blob)], [7, 8])
        no_log, covered = adapter_log.region_from_uf2(blocks[0])
        self.assertEqual(covered, 0)

    def test_corrupt_header_ignored(self):
        blob = bytearray(self.blob)
        blob[3 * adapter_log.SLOT_SIZE + 6] ^= 1
        self.assertEqual([b["boot"] for b in adapter_log.parse_dump(bytes(blob))], [8])


class AuthHelpers(unittest.TestCase):
    def test_nonce_pages(self):
        nonce = bytes(range(256))
        pages = auth_probe.nonce_pages(nonce, 0x33)
        self.assertEqual(len(pages), 5)
        for n, p in enumerate(pages):
            self.assertEqual(len(p), 64)
            self.assertEqual(p[:4], bytes([0xF0, 0x33, n, 0]))
            self.assertEqual(struct.unpack_from("<I", p, 60)[0], binascii.crc32(p[:60]) & 0xFFFFFFFF)
        self.assertEqual(b"".join(p[4:60] for p in pages)[:256], nonce)

    def test_signature_verification(self):
        try:
            from cryptography.hazmat.primitives import hashes
            from cryptography.hazmat.primitives.asymmetric import padding, rsa
        except ImportError:
            self.skipTest("cryptography not installed")
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        nonce = os.urandom(256)
        sig = key.sign(nonce, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        nums = key.public_key().public_numbers()
        resp = sig + b"S" * 16 + nums.n.to_bytes(256, "big") + nums.e.to_bytes(256, "big") + b"\0" * 256
        ok, _ = auth_probe.verify(nonce, auth_probe.split_response(resp))
        self.assertTrue(ok)
        ok, _ = auth_probe.verify(os.urandom(256), auth_probe.split_response(resp))
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
