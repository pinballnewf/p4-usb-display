"""Host side of the P4 USB display link (see firmware/p4usbdisp/main/usb_link.h)."""
import io
import struct

import usb.core
import usb.util

VID, PID = 0x303A, 0x4020
EP_OUT = 0x01

MAGIC = 0x31443450  # "P4D1"
TYPE_JPEG_FULL = 1
REQ_GET_STATS = 0x01
REQ_BACKLIGHT = 0x02

PANEL_W, PANEL_H = 800, 1280  # native portrait raster

_LINK_STATS = struct.Struct("<QIIIII")
_DEC_STATS = struct.Struct("<IIII")
_AUDIO_STATS = struct.Struct("<IIIHHII")


class P4Display:
    def __init__(self):
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is None:
            raise RuntimeError(f"no {VID:04x}:{PID:04x} device - is the middle USB-C port connected?")
        # A port reset drops any half-received message on the device and
        # re-arms its header read, so every session starts in sync.
        dev.reset()
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        # Only configure an unconfigured device: once the kernel's hid-multitouch
        # has bound the touch interface, SET_CONFIGURATION fails with EBUSY.
        try:
            configured = dev.get_active_configuration() is not None
        except usb.core.USBError:
            configured = False
        if not configured:
            dev.set_configuration()
        usb.util.claim_interface(dev, 0)
        self.dev = dev
        self.seq = 0

    @property
    def high_speed(self):
        return self.dev.speed == usb.util.SPEED_HIGH

    def send_jpeg(self, jpeg: bytes):
        hdr = struct.pack("<IBBHII", MAGIC, TYPE_JPEG_FULL, 0, 0, len(jpeg), self.seq)
        self.dev.write(EP_OUT, hdr, timeout=1000)
        self.dev.write(EP_OUT, jpeg, timeout=2000)
        self.seq += 1

    def send_image(self, img, rotate=90, quality=85):
        """img: PIL image in the desktop's orientation. rotate: degrees
        counter-clockwise to reach the panel's portrait raster (0, 90, 180, 270)."""
        self.send_jpeg(encode(img, rotate, quality))

    def backlight(self, percent: int):
        self.dev.ctrl_transfer(0x41, REQ_BACKLIGHT, max(0, min(100, percent)), 0, None, timeout=1000)

    def stats(self):
        raw = bytes(self.dev.ctrl_transfer(0xC1, REQ_GET_STATS, 0, 0, 128, timeout=1000))
        link = dict(zip(("rx_bytes", "frames_rx", "frames_stale", "bad_header", "truncated", "oversize"),
                        _LINK_STATS.unpack_from(raw)))
        if len(raw) >= _LINK_STATS.size + _DEC_STATS.size:
            link.update(zip(("frames_shown", "decode_fail", "decode_us_last", "decode_us_avg"),
                            _DEC_STATS.unpack_from(raw, _LINK_STATS.size)))
        off = _LINK_STATS.size + _DEC_STATS.size
        if len(raw) >= off + _AUDIO_STATS.size:
            link.update(zip(("audio_streams", "audio_underruns", "audio_chunks", "audio_fifo_min",
                             "audio_fifo_max", "audio_rx_packets", "audio_rx_bytes"),
                            _AUDIO_STATS.unpack_from(raw, off)))
        return link


def encode(img, rotate=90, quality=85):
    """Rotate into the panel raster and encode as baseline 4:2:0 JPEG - the
    board's decoder hangs on 4:2:2 (see docs/DEVELOPMENT.md)."""
    if rotate:
        img = img.rotate(rotate, expand=True)
    if img.size != (PANEL_W, PANEL_H):
        raise ValueError(f"after rotation image is {img.size}, panel needs {(PANEL_W, PANEL_H)}")
    buf = io.BytesIO()
    img.convert("RGB").save(buf, "JPEG", quality=quality, subsampling=2, optimize=False)
    return buf.getvalue()
