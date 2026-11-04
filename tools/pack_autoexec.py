#!/usr/bin/env python3
"""Pack autoexec.sh + rbp_inject + rrfx_ui100.so into an XDJ-RR autoexec.bin.

Part of RRFX. Copyright (C) 2026 Johan Li and contributors.
License: GPL-3.0-or-later.

The XDJ-RR (firmware 1.08) runs an autoexec.bin found on an inserted USB stick.
The file is an ISO 9660 image (Rock Ridge), encrypted with AES-256-CBC in
512-byte sectors, IV = little-endian sector index + 12 zero bytes.

THE KEY IS NOT PART OF THIS PROJECT AND MUST NEVER BE COMMITTED OR PUBLISHED.
You supply your own key file (--key). It is only read, never copied or printed.
Its first line is used as the unit's loop-device passphrase: the first 31 bytes
followed by a NUL byte form the 32-byte AES key.

Requires: python3, cryptography, pycdlib (pip install cryptography pycdlib).
"""
import argparse
import io
import struct
import sys

SECTOR = 512
ISO_BLOCK = 2048


def load_key(path):
    raw = open(path, "rb").read()
    first = raw.split(b"\n", 1)[0]
    if len(first) < 31 or b"\0" in first[:31]:
        sys.exit("key file: unexpected format (need a first line of at least 31 bytes)")
    return first[:31] + b"\0"


def crypt_sectors(data, key, decrypt=False, first_sector=0):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    if len(data) % SECTOR or len(key) != 32:
        raise ValueError("need sector-aligned data and a 32-byte key")
    out = bytearray()
    for index, off in enumerate(range(0, len(data), SECTOR), first_sector):
        iv = struct.pack("<I", index) + bytes(12)
        c = Cipher(algorithms.AES(key), modes.CBC(iv))
        op = c.decryptor() if decrypt else c.encryptor()
        out += op.update(data[off:off + SECTOR]) + op.finalize()
    return bytes(out)


def check_key_against_updater(key, updater_path):
    """Optional: prove the key is right by decoding the official XDJRR.UPD's
    ISO volume descriptor (sector 16 of the image)."""
    upd = open(updater_path, "rb").read()
    start = 16 * ISO_BLOCK
    pvd = crypt_sectors(upd[start:start + ISO_BLOCK], key, decrypt=True,
                        first_sector=start // SECTOR)
    if pvd[:7] != b"\x01CD001\x01" or pvd[40:72].rstrip(b" ") != b"UsbAuto":
        sys.exit("key check FAILED: this key does not decode the official 1.08 updater")
    print("key check: OK (decodes the official updater)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--key", required=True, help="your own XDJ-RR 1.08 key file (never published)")
    ap.add_argument("--script", required=True, help="autoexec.sh")
    ap.add_argument("--injector", required=True, help="rbp_inject")
    ap.add_argument("--library", required=True, help="rrfx_ui100.so")
    ap.add_argument("--out", required=True, help="output autoexec.bin")
    ap.add_argument("--check-updater", help="optional: official XDJRR.UPD 1.08 to verify the key")
    a = ap.parse_args()

    import pycdlib
    key = load_key(a.key)
    if a.check_updater:
        check_key_against_updater(key, a.check_updater)

    files = [("/AUTOEXEC.SH;1", "autoexec.sh", 0o100555, a.script),
             ("/RBPINJ.BIN;1", "rbp_inject", 0o100555, a.injector),
             ("/RRFX100.BIN;1", "rrfx_ui100.so", 0o100644, a.library)]
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="UsbAuto", rock_ridge="1.09", sys_ident="LINUX")
    blobs = {}
    for iso_path, rr_name, mode, path in files:
        data = open(path, "rb").read()
        blobs[rr_name] = data
        iso.add_fp(io.BytesIO(data), len(data), iso_path=iso_path, rr_name=rr_name, file_mode=mode)
    buf = io.BytesIO()
    iso.write_fp(buf)
    iso.close()
    enc = crypt_sectors(buf.getvalue(), key)

    # Round trip: decrypt, reopen, compare every file and its mode.
    chk = pycdlib.PyCdlib()
    chk.open_fp(io.BytesIO(crypt_sectors(enc, key, decrypt=True)))
    for _, rr_name, mode, _ in files:
        got = io.BytesIO()
        chk.get_file_from_iso_fp(got, rr_path="/" + rr_name)
        assert got.getvalue() == blobs[rr_name], rr_name
        assert chk.get_record(rr_path="/" + rr_name).rock_ridge.get_file_mode() == mode, rr_name
    chk.close()
    open(a.out, "wb").write(enc)
    print("autoexec.bin: %d bytes, round trip verified" % len(enc))


if __name__ == "__main__":
    main()
