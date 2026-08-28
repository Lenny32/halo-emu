#!/usr/bin/env python3
"""rom_map_check.py — verify a firmware image links against the ROM
symbol map the synthetic ROM stub implements (ticket 0034, task 3).

The Halo's BLE host stack and LC3 codec live in on-chip ROM and are
called through pinned addresses; this repo's stub (rom-stub/, ticket
0028) is built against the vendored **v1_2** maps.  If a firmware
release moves to another ROM image version every one of those addresses
shifts, and the failure mode is a nonsense crash deep inside the stub.
This turns that into one legible message, before the machine starts.

How: the firmware lives in MRAM at 0x80020800 and the ROM at 0x0009xxxx,
much too far for a `BL`, so every ROM call goes through a 32-bit address
literal.  Scan the image for word-aligned little-endian words that land
in the ROM window with the Thumb bit set, and intersect them with the
map.  A firmware built against v1_2 resolves ~38% of them exactly (the
rest are ordinary constants that happen to land in the ROM window); one
built against any other map resolves a small fraction of that, because
ROM entry points do not survive a rebuild at the same address.

    tools/rom_map_check.py -f firmwares/0.8.9/0.8.9.bin
    tools/rom_map_check.py -f fw.bin -v      # also name the symbols hit

Exit 0 = compatible (or nothing to check), 1 = mismatch.  --require-rom
also fails the nothing-to-check case, which is what CI wants on an
application image.
"""

import argparse
import glob
import os
import re
import struct
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LDS_GLOB = os.path.join(REPO_ROOT, "rom-stub", "vendor", "lds",
                        "rom_symbols_*_v1_2.lds")
ROM_MAP_VERSION = "v1_2"

# The ROM window the pinned symbols live in (0x9f095..0x140d55 in v1_2),
# rounded out so a moved map is still inside the search range and gets
# counted as a candidate rather than silently ignored.
ROM_LO, ROM_HI = 0x00090000, 0x00160000

# Calibration.  0.8.8 and 0.8.9 (release, debug and signed assets alike)
# each reference 73 distinct v1_2 entry points out of ~194 ROM-window
# literals — 37-38%, the rest being ordinary constants that happen to
# land in the window.  The bare-metal test firmwares in rom-stub/test/
# call a handful of ROM functions and score 93%.  So the signal is the
# *ratio*, not the count: a count threshold would reject those small
# images, and would still pass a near miss (shifting every ROM literal
# by one entry, +0x10, lands 25 of them on some other real symbol —
# 13%, which the ratio catches and a count of 25 does not).
MIN_HIT_RATIO = 0.25
# Below this many ROM-window literals there is no signal either way.
MIN_REFS = 8

SYM_RE = re.compile(r"^\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;")


def load_symbols(paths=None):
    """{address: name} from the vendored ROM symbol maps."""
    paths = paths or sorted(glob.glob(LDS_GLOB))
    if not paths:
        raise SystemExit(f"rom-map-check: no symbol maps at {LDS_GLOB}")
    syms = {}
    for path in paths:
        with open(path) as f:
            for line in f:
                m = SYM_RE.match(line)
                if m:
                    syms[int(m.group(2), 16)] = m.group(1)
    return syms


def rom_references(firmware):
    """Word-aligned Thumb addresses in the image that point at ROM."""
    out = set()
    for off in range(0, len(firmware) - 3, 4):
        word = struct.unpack_from("<I", firmware, off)[0]
        if ROM_LO <= word < ROM_HI and (word & 1):
            out.add(word)
    return out


def check(image, symbols=None):
    """Classify a firmware image against the vendored ROM map.

    Returns (verdict, message, hits, refs) where verdict is:
      "ok"        - it links against v1_2; the stub matches
      "none"      - it makes no ROM calls at all (the bare-metal test
                    firmwares in rom-stub/test/, or an app built without
                    the ROM BLE/LC3 stack).  Nothing to check.
      "mismatch"  - it calls into ROM at addresses this map does not
                    know: a different on-chip ROM image version.
    """
    symbols = symbols if symbols is not None else load_symbols()
    refs = rom_references(image)
    hits = {a: symbols[a] for a in refs if a in symbols}
    ratio = len(hits) / len(refs) if refs else 0.0

    if len(refs) >= MIN_REFS and ratio >= MIN_HIT_RATIO:
        return ("ok",
                f"ROM map {ROM_MAP_VERSION}: {len(hits)} of {len(refs)} "
                f"ROM-window references ({ratio:.0%}) resolve to pinned "
                f"symbols",
                hits, refs)
    if len(refs) < MIN_REFS:
        return ("none",
                f"only {len(refs)} ROM-window address literals — the "
                "image makes no meaningful on-chip ROM calls, nothing "
                "to check",
                hits, refs)
    return ("mismatch",
            f"ROM symbol map mismatch: only {len(hits)} of {len(refs)} "
            f"ROM-window address literals ({ratio:.0%}) match the "
            f"vendored {ROM_MAP_VERSION} map (a firmware built against "
            f"it matches at least {MIN_HIT_RATIO:.0%}, in practice "
            f"38%).\n"
            "  The synthetic ROM stub (rom-stub/) implements ROM symbol "
            f"map {ROM_MAP_VERSION} only, so this release's BLE/LC3 "
            "calls would land on\n"
            "  the wrong stub entry points.  Rebuild the stub against "
            "that release's modules/hal/alif/ble/<ver>/"
            "rom_symbols_*.lds\n"
            "  (see rom-stub/README.md), or run with "
            "--no-rom-map-check to try anyway.",
            hits, refs)


def main():
    p = argparse.ArgumentParser(
        prog="rom_map_check.py",
        description="Check a firmware image against the ROM symbol map "
                    "the synthetic ROM stub implements.")
    p.add_argument("-f", "--firmware", required=True, metavar="BIN")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="list the ROM symbols the image references")
    p.add_argument("--require-rom", action="store_true",
                   help="also fail when the image makes no ROM calls at "
                        "all — CI uses this on a released application "
                        "image, where 'no ROM calls' is itself wrong")
    args = p.parse_args()

    try:
        image = open(args.firmware, "rb").read()
    except OSError as e:
        sys.exit(f"rom-map-check: cannot read firmware: {e}")

    verdict, msg, hits, _ = check(image)
    bad = verdict == "mismatch" or (verdict == "none" and args.require_rom)
    print(f"rom-map-check: {os.path.basename(args.firmware)}: {msg}",
          file=sys.stderr if bad else sys.stdout)
    if args.verbose:
        for addr, name in sorted(hits.items()):
            print(f"  {addr:#08x}  {name}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
