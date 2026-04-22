#!/usr/bin/env python3
"""Validate a Pokemon Snap N64 ROM, normalize to big-endian .z64, and
record its SHA-1.

Usage:
    python emu/tools/extract_rom.py <path-to-rom> <nphe|npfe>

The first run on a given variant will print the observed SHA-1 and exit
non-zero with instructions for pinning it into EXPECTED_SHA1 below. This
is deliberate: we do not guess canonical hashes for ROMs we have not
seen, so the first commit after a successful extract records the pin.

Supported input formats:
  * z64 - big-endian  (native; magic 0x80371240)
  * n64 - little-endian (magic 0x40123780)
  * v64 - byte-swapped  (magic 0x37804012)

Output is always a .z64 (big-endian) in the same directory.
"""
from __future__ import annotations

import hashlib
import sys
import struct
from pathlib import Path

MAGIC_Z64 = 0x80371240
MAGIC_N64 = 0x40123780
MAGIC_V64 = 0x37804012

# Pin known-good SHA-1 hashes here after the first successful extract.
# Leave None until the user confirms. Do not populate from external
# databases without user confirmation.
EXPECTED_SHA1: dict[str, str | None] = {
    "nphe": None,   # kiosk Snap Station ROM (US)
    "npfe": None,   # retail Pokemon Snap (US)
}


def detect_and_normalize(data: bytes) -> bytes:
    if len(data) < 4:
        raise ValueError("file too small to be a ROM")
    magic = struct.unpack(">I", data[:4])[0]
    if magic == MAGIC_Z64:
        return data
    if magic == MAGIC_N64:
        # swap every 4 bytes
        return b"".join(
            data[i:i+4][::-1] for i in range(0, len(data), 4)
        )
    if magic == MAGIC_V64:
        # swap every 2 bytes
        return b"".join(
            data[i:i+2][::-1] for i in range(0, len(data), 2)
        )
    raise ValueError(f"unknown ROM magic 0x{magic:08X}")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    rom_path = Path(sys.argv[1])
    variant = sys.argv[2].lower()
    if variant not in EXPECTED_SHA1:
        print(f"unknown variant {variant!r}; choose from {list(EXPECTED_SHA1)}",
              file=sys.stderr)
        return 2
    if not rom_path.is_file():
        print(f"not a file: {rom_path}", file=sys.stderr)
        return 2

    raw = rom_path.read_bytes()
    try:
        z64 = detect_and_normalize(raw)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    sha1 = hashlib.sha1(z64).hexdigest()
    out_path = rom_path.with_suffix(".z64")
    if out_path != rom_path:
        out_path.write_bytes(z64)
        print(f"wrote normalized .z64 to {out_path}")

    expected = EXPECTED_SHA1[variant]
    if expected is None:
        print(f"observed SHA-1 for {variant}: {sha1}")
        print("First run: pin this hash into EXPECTED_SHA1 in this script,")
        print("commit the change, then re-run. This prevents silent ROM drift.")
        return 1
    if sha1 != expected:
        print(f"SHA-1 mismatch for {variant}: expected {expected}, got {sha1}",
              file=sys.stderr)
        return 1
    print(f"{variant} OK (SHA-1 {sha1})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
