"""Minimal EDID 1.4 base block for the P4 USB display's virtual EVDI connector."""
import math
import struct

PANEL_MM = (135, 217)  # active area, portrait (216.58 x 135.36 mm per the spec sheet)


def _mfg_id(code: str) -> bytes:
    a, b, c = (ord(ch) - ord("A") + 1 for ch in code)
    return struct.pack(">H", (a << 10) | (b << 5) | c)


def _text_descriptor(tag: int, text: str) -> bytes:
    body = text.encode("ascii")[:13]
    if len(body) < 13:
        body += b"\n" + b" " * (12 - len(body))
    return bytes([0, 0, 0, tag, 0]) + body


def _cvt_rb_dtd(w: int, h: int, hz: float, mm_w: int, mm_h: int) -> bytes:
    """Detailed timing descriptor using CVT reduced-blanking v1 parameters."""
    hfp, hsync, hbp = 48, 32, 80
    htotal = w + hfp + hsync + hbp
    vfp, vsync = 3, 10
    # CVT-RB: minimum 460 us of vertical blanking.
    vtotal = math.ceil(h / (1 - 460e-6 * hz))
    vblank = vtotal - h
    pclk_10khz = round(htotal * vtotal * hz / 10_000)
    hblank = htotal - w

    return struct.pack(
        "<H16B",
        pclk_10khz,
        w & 0xFF, hblank & 0xFF, ((w >> 8) << 4) | (hblank >> 8),
        h & 0xFF, vblank & 0xFF, ((h >> 8) << 4) | (vblank >> 8),
        hfp & 0xFF, hsync & 0xFF, ((vfp & 0xF) << 4) | (vsync & 0xF),
        ((hfp >> 8) << 6) | ((hsync >> 8) << 4) | ((vfp >> 4) << 2) | (vsync >> 4),
        mm_w & 0xFF, mm_h & 0xFF, ((mm_w >> 8) << 4) | (mm_h >> 8),
        0, 0,
        0x1A,  # digital separate sync, hsync +, vsync - (CVT-RB)
    )


def build(width: int, height: int, hz: float = 60.0, name: str = "P4 USB Panel", serial: str = "000001") -> bytes:
    mm_w, mm_h = PANEL_MM if width < height else PANEL_MM[::-1]
    e = bytearray()
    e += b"\x00\xff\xff\xff\xff\xff\xff\x00"
    e += _mfg_id("LFR")
    e += struct.pack("<HI", 0x4020, 1)
    e += bytes([39, 2026 - 1990])          # week, year
    e += bytes([1, 4])                      # EDID 1.4
    e += bytes([0xA0])                      # digital, 8 bpc
    e += bytes([round(mm_w / 10), round(mm_h / 10)])
    e += bytes([120])                       # gamma 2.2
    e += bytes([0x06])                      # sRGB default, preferred timing in DTD 1
    e += bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])  # sRGB chromaticity
    e += bytes([0, 0, 0])                   # no established timings
    e += b"\x01\x01" * 8                    # no standard timings
    e += _cvt_rb_dtd(width, height, hz, mm_w, mm_h)
    e += _text_descriptor(0xFC, name)
    e += bytes([0, 0, 0, 0xFD, 0, 50, 75, 30, 160, 20, 0x01, 0x0A]) + b" " * 6  # range limits
    e += _text_descriptor(0xFF, serial)
    e += bytes([0])                         # no extensions
    e += bytes([(-sum(e)) & 0xFF])
    assert len(e) == 128
    return bytes(e)


if __name__ == "__main__":
    import sys
    w, h = (1280, 800) if "--landscape" in sys.argv else (800, 1280)
    sys.stdout.buffer.write(build(w, h))
