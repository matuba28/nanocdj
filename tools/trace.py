#!/usr/bin/env python3
"""Read the NanoApps trace ring from a running iPod WITHOUT disturbing it.

NanoApps' `./start trace` on macOS first looks for the iPod's disk, and when the
iPod isn't mounted (which is the case while an app is running) it sends a SCSI
"load media" command. That puts the iPod into sync mode, which closes the app and
then reboots it ~20 s later, wiping the very log we wanted.

This reader only uses the read-only C6/98 memory peek through ipod_sg_raw's
"auto" device selection, the same call as NanoApps' MacDevice.peek_ejected
("read without loading media"). It never mounts, unmounts or loads media.

macOS only (it uses NanoApps' IOKit SCSI helper, built here on first use with
the Xcode Command Line Tools). The iPod must be plugged in and on its normal
screen, not in disk mode.

Usage: tools/trace.py [COUNT] [--only PREFIX]     (default: last 64 entries)
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NANOAPPS = Path(os.environ.get("NANOAPPS", ROOT / "third_party/NanoApps"))
SG_SRC = NANOAPPS / "tools/macos/ipod_sg_raw.c"
SG_RAW = ROOT / "tools/.build/ipod_sg_raw"


def ensure_sg_raw():
    """Build NanoApps' read-only SCSI helper into tools/.build (same flags as its ./start)."""
    if SG_RAW.is_file() and SG_RAW.stat().st_mtime >= SG_SRC.stat().st_mtime:
        return
    if not SG_SRC.is_file():
        raise SystemExit(f"{SG_SRC} not found: run `git submodule update --init`")
    SG_RAW.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["xcrun", "clang", "-O2", "-Wno-deprecated-declarations", "-framework", "IOKit",
                    "-framework", "CoreFoundation", "-o", str(SG_RAW), str(SG_SRC)], check=True)
BOOT_REPORT = 0x086C9B84
TRACE_MAGIC = 0x48425452  # 'RTBH'


def peek(address, size):
    data = bytearray()
    with tempfile.TemporaryDirectory(prefix="nanocdj-peek-") as td:
        path = Path(td) / "block"
        while len(data) < size:
            addr = address + len(data)
            base = addr & ~511
            subprocess.run([str(SG_RAW), "-r", "512", "-o", str(path), "auto", "c6", "98", "02",
                            *(f"{b:02x}" for b in base.to_bytes(4, "big"))],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            block = path.read_bytes()
            if len(block) != 512:
                raise SystemExit("short read from the iPod")
            data.extend(block[addr - base:addr - base + size - len(data)])
    return bytes(data)


def main():
    args = sys.argv[1:]
    only = None
    if "--only" in args:
        i = args.index("--only"); only = args[i + 1]; del args[i:i + 2]
    want = int(args[0]) if args else 64
    ensure_sg_raw()
    report = peek(BOOT_REPORT, 512)
    if struct.unpack_from("<I", report, 0x2C)[0] != 0x32505452:
        raise SystemExit("no boot report: is the iPod attached and on NanoApps firmware?")
    address = struct.unpack_from("<I", report, 0x150)[0]
    if not 0x08D00000 <= address < 0x0A000000 - 4112:
        raise SystemExit("the resident hasn't published a trace ring")
    data = peek(address, 4112)
    magic, idx, count, _ = struct.unpack_from("<IIII", data, 0)
    if magic != TRACE_MAGIC:
        raise SystemExit("trace ring not initialised yet")
    cap = (len(data) - 16) // 16
    n = min(count, cap)
    rows = []
    for s in [(idx - 1 - k) % cap for k in range(n)][::-1]:
        o = 16 + s * 16
        tag = data[o:o + 8].rstrip(b" \x00").decode("ascii", "replace")
        v1, v2 = struct.unpack_from("<II", data, o + 8)
        if only and not tag.startswith(only):
            continue
        rows.append(f"{tag:<8} 0x{v1:08x} 0x{v2:08x}")
    print(f"HBTR idx={idx} count={count}")
    print("\n".join(rows[-want:]))


if __name__ == "__main__":
    main()
