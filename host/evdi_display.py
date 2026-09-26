#!/usr/bin/env python3
"""Present the P4 USB display to the desktop as a real monitor, via EVDI.

EVDI adds a virtual DRM connector; the compositor (KWin here) treats it like any
plugged-in monitor. This daemon hands it the panel's EDID, receives every frame
the compositor renders for it, encodes 4:2:0 JPEG and ships it over USB.

By default the EDID advertises the panel's native 800x1280 portrait raster and
the compositor rotates the output (KDE remembers the orientation per monitor),
so rotation happens on the GPU and frames arrive already in panel order.
--rotate-output asks KDE to set landscape on first use. --landscape advertises
1280x800 instead and rotates on the CPU (~6 ms/frame).

Needs: evdi kernel module loaded with a free device (see system/), libevdi1,
simplejpeg, and access to the evdi card node (system/70-evdi-uaccess.rules).

Usage: evdi_display.py [--landscape] [--quality Q] [--max-fps N] [--rotate-output]
"""
import argparse
import ctypes as C
import select
import signal
import shutil
import subprocess
import sys
import time

import numpy as np
import simplejpeg

import edid
from p4disp import PANEL_H, PANEL_W, P4Display

# ---- libevdi bindings (evdi_lib.h, 1.14.x) ----

AVAILABLE, UNRECOGNIZED, NOT_PRESENT = 0, 1, 2
MAX_DIRTS = 16


class Rect(C.Structure):
    _fields_ = [("x1", C.c_int), ("y1", C.c_int), ("x2", C.c_int), ("y2", C.c_int)]


class Mode(C.Structure):
    _fields_ = [("width", C.c_int), ("height", C.c_int), ("refresh_rate", C.c_int),
                ("bits_per_pixel", C.c_int), ("pixel_format", C.c_uint)]


class Buffer(C.Structure):
    _fields_ = [("id", C.c_int), ("buffer", C.c_void_p), ("width", C.c_int), ("height", C.c_int),
                ("stride", C.c_int), ("rects", C.POINTER(Rect)), ("rect_count", C.c_int)]


DPMS_CB = C.CFUNCTYPE(None, C.c_int, C.c_void_p)
MODE_CB = C.CFUNCTYPE(None, Mode, C.c_void_p)
UPDATE_CB = C.CFUNCTYPE(None, C.c_int, C.c_void_p)
CRTC_CB = C.CFUNCTYPE(None, C.c_int, C.c_void_p)


class EventContext(C.Structure):
    _fields_ = [("dpms_handler", DPMS_CB), ("mode_changed_handler", MODE_CB),
                ("update_ready_handler", UPDATE_CB), ("crtc_state_handler", CRTC_CB),
                ("cursor_set_handler", C.c_void_p), ("cursor_move_handler", C.c_void_p),
                ("ddcci_data_handler", C.c_void_p), ("user_data", C.c_void_p)]


lib = C.CDLL("libevdi.so.1")
lib.evdi_check_device.argtypes = [C.c_int]
lib.evdi_check_device.restype = C.c_int
lib.evdi_open.argtypes = [C.c_int]
lib.evdi_open.restype = C.c_void_p
lib.evdi_close.argtypes = [C.c_void_p]
lib.evdi_connect2.argtypes = [C.c_void_p, C.c_char_p, C.c_uint, C.c_uint32, C.c_uint32]
lib.evdi_disconnect.argtypes = [C.c_void_p]
lib.evdi_enable_cursor_events.argtypes = [C.c_void_p, C.c_bool]
lib.evdi_grab_pixels.argtypes = [C.c_void_p, C.POINTER(Rect), C.POINTER(C.c_int)]
lib.evdi_register_buffer.argtypes = [C.c_void_p, Buffer]
lib.evdi_unregister_buffer.argtypes = [C.c_void_p, C.c_int]
lib.evdi_request_update.argtypes = [C.c_void_p, C.c_int]
lib.evdi_request_update.restype = C.c_bool
lib.evdi_handle_events.argtypes = [C.c_void_p, C.POINTER(EventContext)]
lib.evdi_get_event_ready.argtypes = [C.c_void_p]
lib.evdi_get_event_ready.restype = C.c_int


def open_evdi():
    for i in range(64):
        status = lib.evdi_check_device(i)
        if status == AVAILABLE:
            h = lib.evdi_open(i)
            if h:
                return i, h
            sys.exit(f"evdi card{i} is available but could not be opened - "
                     "is system/70-evdi-uaccess.rules installed?")
    sys.exit("no free EVDI device. Load the module with a device:\n"
             "  sudo modprobe evdi initial_device_count=1\n"
             "(system/evdi-modprobe.conf makes that permanent)")


# ---- Daemon ----

BUF_ID = 1


class Stats:
    """Per-stage host timings over a reporting window: average / worst, in ms.
    wait = update ready -> grab starts, total = update ready -> USB send done."""
    STAGES = ("wait", "grab", "enc", "send", "total")

    def __init__(self):
        self.n = 0
        self.bytes = 0
        self.sum = dict.fromkeys(self.STAGES, 0.0)
        self.max = dict.fromkeys(self.STAGES, 0.0)

    def add(self, wait, grab, enc, send, total, nbytes):
        self.n += 1
        self.bytes += nbytes
        for k, v in zip(self.STAGES, (wait, grab, enc, send, total)):
            self.sum[k] += v
            self.max[k] = max(self.max[k], v)

    def line(self, secs):
        parts = " ".join(f"{k} {self.sum[k] / self.n * 1000:4.1f}/{self.max[k] * 1000:4.0f}"
                         for k in self.STAGES)
        return f"{self.n / secs:5.1f} fps {self.bytes / self.n / 1024:4.0f} KB | ms avg/max: {parts}"


class EvdiDisplay:
    def __init__(self, args):
        self.args = args
        self.disp = P4Display()
        self.card, self.h = open_evdi()
        self.buf = None
        self.mode = None
        self.rects = (Rect * MAX_DIRTS)()
        self.update_ready = False
        self.t_ready = None  # when the pending update became available
        self.backlight = 80
        self.dpms_on = True

        self.ctx = EventContext()
        # Keep the ctypes callback objects alive for the lifetime of the context.
        self._cbs = (DPMS_CB(self._on_dpms), MODE_CB(self._on_mode),
                     UPDATE_CB(self._on_update), CRTC_CB(lambda s, u: None))
        self.ctx.dpms_handler, self.ctx.mode_changed_handler, \
            self.ctx.update_ready_handler, self.ctx.crtc_state_handler = self._cbs

    # EVDI callbacks, invoked from evdi_handle_events on this thread.
    def _on_dpms(self, mode, _ud):
        on = mode == 0
        if on != self.dpms_on:
            self.dpms_on = on
            self.disp.backlight(self.backlight if on else 0)
            print(f"monitor power {'on' if on else 'off (backlight off)'}", flush=True)

    def _on_mode(self, mode, _ud):
        print(f"mode set: {mode.width}x{mode.height}@{mode.refresh_rate} "
              f"{mode.bits_per_pixel}bpp fourcc {mode.pixel_format:#x}", flush=True)
        if self.buf is not None:
            lib.evdi_unregister_buffer(self.h, BUF_ID)
        stride = mode.width * 4
        self.buf = np.zeros((mode.height, mode.width, 4), np.uint8)
        b = Buffer(BUF_ID, self.buf.ctypes.data, mode.width, mode.height, stride,
                   C.cast(self.rects, C.POINTER(Rect)), MAX_DIRTS)
        lib.evdi_register_buffer(self.h, b)
        self.mode = (mode.width, mode.height)
        self.update_ready = lib.evdi_request_update(self.h, BUF_ID)

    def _on_update(self, buffer_id, _ud):
        self.update_ready = True
        if self.t_ready is None:
            self.t_ready = time.perf_counter()

    def connect(self):
        w, h = (1280, 800) if self.args.landscape else (PANEL_W, PANEL_H)
        blob = edid.build(w, h)
        lib.evdi_connect2(self.h, blob, len(blob), w * h, w * h * 60)
        # Cursor composited into the frames by the kernel rather than sent separately.
        lib.evdi_enable_cursor_events(self.h, False)
        print(f"EVDI card{self.card} connected as {w}x{h} 'P4 USB Panel'", flush=True)

    def encode(self):
        img = self.buf
        if self.args.landscape:
            img = np.ascontiguousarray(np.rot90(img, 1))
        if img.shape[:2] != (PANEL_H, PANEL_W):
            return None  # compositor picked a mode we cannot show 1:1
        return simplejpeg.encode_jpeg(img, quality=self.args.quality,
                                      colorspace="BGRX", colorsubsampling="420")

    def run(self):
        self.connect()
        fd = lib.evdi_get_event_ready(self.h)
        poller = select.poll()
        poller.register(fd, select.POLLIN)
        min_period = 1.0 / self.args.max_fps
        last_send = 0.0
        t_report = time.perf_counter()
        st = Stats()

        while True:
            # With an update already pending, only wait out the frame-rate cap;
            # otherwise sleep until EVDI signals. Waiting the full timeout with
            # an update pending stalled frames by up to 100 ms.
            if self.update_ready and self.buf is not None:
                timeout_ms = max(0.0, (last_send + min_period - time.perf_counter()) * 1000)
            else:
                timeout_ms = 100
            if poller.poll(timeout_ms):
                lib.evdi_handle_events(self.h, C.byref(self.ctx))
                if self.update_ready and self.t_ready is None:
                    self.t_ready = time.perf_counter()

            now = time.perf_counter()
            if self.update_ready and self.buf is not None and now - last_send >= min_period:
                t0 = now
                t_ready = self.t_ready or t0
                self.update_ready = False
                self.t_ready = None
                nrects = C.c_int(MAX_DIRTS)
                lib.evdi_grab_pixels(self.h, self.rects, C.byref(nrects))
                t1 = time.perf_counter()
                if nrects.value and self.dpms_on:
                    jpeg = self.encode()
                    t2 = time.perf_counter()
                    if jpeg:
                        self.disp.send_jpeg(jpeg)
                        t3 = time.perf_counter()
                        st.add(t0 - t_ready, t1 - t0, t2 - t1, t3 - t2, t3 - t_ready, len(jpeg))
                last_send = t0
                # Ask for the next frame; True means one is already waiting.
                self.update_ready = lib.evdi_request_update(self.h, BUF_ID)
                if self.update_ready:
                    self.t_ready = time.perf_counter()
            elif self.buf is not None and not self.update_ready and now - last_send >= 1.0:
                # Re-arm periodically in case an update request was consumed silently.
                self.update_ready = lib.evdi_request_update(self.h, BUF_ID)
                if self.update_ready:
                    self.t_ready = time.perf_counter()

            if now - t_report >= 5:
                s = self.disp.stats()
                audio = self.audio_line(s)
                if st.n:
                    print(st.line(now - t_report) +
                          f" | board decode {s.get('decode_us_last', 0) / 1000:.1f} ms"
                          f" stale {s['frames_stale']} fail {s.get('decode_fail')}" + audio, flush=True)
                elif audio:
                    print("display idle" + audio, flush=True)
                st = Stats()
                t_report = now

    def audio_line(self, s):
        """Board playback health since the last report, if audio has run."""
        if "audio_chunks" not in s:
            return ""
        prev = getattr(self, "_audio_prev", s)
        self._audio_prev = s
        chunks = s["audio_chunks"] - prev["audio_chunks"]
        if not chunks:
            return ""
        fifo = f"{s['audio_fifo_min']}-{s['audio_fifo_max']}" if s["audio_fifo_max"] else "-"
        pkts = s.get("audio_rx_packets", 0) - prev.get("audio_rx_packets", 0)
        rxb = s.get("audio_rx_bytes", 0) - prev.get("audio_rx_bytes", 0)
        secs = chunks / 1000
        return (f" | audio {secs:.1f} s underruns {s['audio_underruns'] - prev['audio_underruns']}"
                f" fifo {fifo} B rx {pkts / secs:.0f} pkt/s {rxb / secs:.0f} B/s")

    def close(self):
        try:
            lib.evdi_disconnect(self.h)
            lib.evdi_close(self.h)
        except Exception:
            pass


def kscreen_outputs():
    """{connector name: rotation} from kscreen-doctor, or {} if unavailable."""
    import re
    if not shutil.which("kscreen-doctor"):
        return {}
    try:
        out = subprocess.run(["kscreen-doctor", "-o"], capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return {}
    outputs = {}
    for block in re.sub(r"\x1b\[[0-9;]*m", "", out).split("Output: ")[1:]:
        name = block.split()[1]
        m = re.search(r"Rotation: (\d+)", block)
        outputs[name] = int(m.group(1)) if m else 1
    return outputs


def wait_new_output(before, timeout=15.0):
    """The P4's connector name (e.g. DVI-I-1) differs between machines and
    kscreen-doctor does not show monitor names, so it is whichever output
    appeared after we connected. None if kscreen-doctor is unavailable."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new = {k: v for k, v in kscreen_outputs().items() if k not in before}
        if new:
            return next(iter(new.items()))
        time.sleep(0.5)
    return None


def kwin_map_touch(output_name):
    """Tie the board's touchscreen to its monitor in KWin. An external USB
    touchscreen is otherwise left unmapped and its touches land on the wrong
    screen. KWin saves the mapping, so this is a no-op after the first run."""
    try:
        import gi
        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib
    except (ImportError, ValueError):
        return
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION)

        def call(path, iface, method, args=None, ret=None):
            return bus.call_sync("org.kde.KWin", path, iface, method, args,
                                 GLib.VariantType(ret) if ret else None,
                                 Gio.DBusCallFlags.NONE, 2000, None)

        def prop(path, name):
            return call(path, "org.freedesktop.DBus.Properties", "Get",
                        GLib.Variant("(ss)", ("org.kde.KWin.InputDevice", name)), "(v)").unpack()[0]

        devices = call("/org/kde/KWin/InputDevice", "org.kde.KWin.InputDeviceManager",
                       "ListTouch", None, "(as)").unpack()[0]
        for dev in devices:
            path = f"/org/kde/KWin/InputDevice/{dev}"
            if "P4 USB Display" not in prop(path, "name"):
                continue
            if prop(path, "outputName") != output_name:
                call(path, "org.freedesktop.DBus.Properties", "Set",
                     GLib.Variant("(ssv)", ("org.kde.KWin.InputDevice", "outputName",
                                            GLib.Variant("s", output_name))))
                print(f"mapped touchscreen {dev} to {output_name}", flush=True)
    except GLib.Error:
        pass  # not KWin, or no session bus: nothing to map


def desktop_setup(before, rotate):
    """First-run conveniences once the compositor has picked up the monitor."""
    found = wait_new_output(before)
    if not found:
        return
    name, rotation = found
    if rotate and rotation == 1:  # 1 = none; left = 90 degrees counter-clockwise
        subprocess.run(["kscreen-doctor", f"output.{name}.rotation.left"], timeout=5)
        print(f"asked KDE to rotate {name} to landscape", flush=True)
    kwin_map_touch(name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--landscape", action="store_true",
                    help="advertise 1280x800 and rotate on the CPU instead of in the compositor")
    ap.add_argument("--rotate-output", action="store_true",
                    help="ask KDE to rotate the portrait output to landscape if it is unrotated "
                         "(first-time setup; afterwards KDE's remembered orientation wins)")
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--max-fps", type=float, default=60)
    args = ap.parse_args()

    before = kscreen_outputs()
    d = EvdiDisplay(args)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    import threading
    threading.Thread(target=desktop_setup, args=(before, args.rotate_output and not args.landscape),
                     daemon=True).start()
    try:
        d.run()
    except KeyboardInterrupt:
        pass
    finally:
        d.close()


if __name__ == "__main__":
    main()
