#!/usr/bin/env python3
"""
Extract a JD9365 init sequence out of a Guition full-flash factory image.

The vendor firmware uses the same esp_lcd_jd9365 driver we do, so its init
table is an array of:
    { int cmd; const void *data; size_t data_bytes; unsigned delay_ms; }
= 16 bytes per entry, with data pointing into a loaded segment.

To read the data bytes we have to map those virtual addresses back to file
offsets, which means parsing the app image's segment table.
"""
import struct
import sys

FLASH_IMG = sys.argv[1]
TABLE_OFF = int(sys.argv[2], 0)
ENTRY = 16


def read(path):
    return open(path, "rb").read()


def parse_partitions(d):
    """Partition table lives at 0x8000; 32-byte entries, magic 0xAA50."""
    parts = []
    for off in range(0x8000, 0x9000, 32):
        e = d[off:off + 32]
        if e[:2] != b"\xaa\x50":
            break
        ptype, subtype, addr, size = struct.unpack("<BBII", e[2:12])
        label = e[12:28].rstrip(b"\x00").decode("ascii", "replace")
        parts.append((label, ptype, subtype, addr, size))
    return parts


def segment_map(d, app_off):
    """Return [(vaddr, file_off, length)] for the app image's segments."""
    magic, nseg = d[app_off], d[app_off + 1]
    if magic != 0xE9:
        return []
    # 24-byte extended header on ESP32-P4 images
    p = app_off + 24
    segs = []
    for _ in range(nseg):
        vaddr, length = struct.unpack("<II", d[p:p + 8])
        p += 8
        segs.append((vaddr, p, length))
        p += length
    return segs


def resolve(segs, vaddr, n):
    for base, foff, length in segs:
        if base <= vaddr < base + length:
            o = foff + (vaddr - base)
            return o, n
    # Not in any loaded segment. If it points at internal RAM it is almost
    # certainly .bss, which has no file backing because it is zero-filled at
    # startup - so the constant really is 0x00.
    if 0x4FF00000 <= vaddr < 0x50000000 or 0x30100000 <= vaddr < 0x30200000:
        return "BSS", n
    return None, 0


def main():
    d = read(FLASH_IMG)

    app_off = None
    for label, ptype, subtype, addr, size in parse_partitions(d):
        if ptype == 0:  # app
            app_off = addr
            print(f"# app partition '{label}' at 0x{addr:06x} ({size} bytes)")
            break
    if app_off is None:
        print("# no app partition found", file=sys.stderr)
        return 1

    segs = segment_map(d, app_off)
    print(f"# {len(segs)} segments")
    for v, f, l in segs:
        print(f"#   vaddr 0x{v:08x}  file 0x{f:08x}  len {l}")

    entries = []
    off = TABLE_OFF
    while off + ENTRY <= len(d):
        cmd, ptr, nbytes, delay = struct.unpack("<IIII", d[off:off + ENTRY])
        if cmd > 0xFF or nbytes == 0 or nbytes > 32 or delay > 1000:
            break
        foff, n = resolve(segs, ptr, nbytes)
        if foff is None:
            break
        data = bytes(n) if foff == "BSS" else d[foff:foff + n]
        entries.append((cmd, data, delay))
        off += ENTRY

    print(f"# recovered {len(entries)} init commands\n")
    for cmd, data, delay in entries:
        vals = ", ".join(f"0x{b:02X}" for b in data)
        print(f"    {{0x{cmd:02X}, (uint8_t []){{{vals}}}, {len(data)}, {delay}}},")
    return 0


sys.exit(main())
