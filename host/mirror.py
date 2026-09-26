#!/usr/bin/env python3
"""Mirror a monitor or window to the P4 USB display (Wayland, via xdg-desktop-portal).

The first run shows the desktop's screen-share picker; the choice is remembered
(portal restore token) so later runs start straight away. --pick asks again.

Pipeline: portal ScreenCast -> PipeWire -> GStreamer (scale to 1280x800 with
letterbox, rotate into the panel's portrait raster, 4:2:0 JPEG) -> USB.

Usage: mirror.py [--pick] [--window] [--quality Q] [--rotate 90|270] [--max-fps N]
"""
import argparse
import os
import random
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gio, GLib, Gst  # noqa: E402

from p4disp import PANEL_H, PANEL_W, P4Display  # noqa: E402

PORTAL = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SCREENCAST = "org.freedesktop.portal.ScreenCast"
TOKEN_FILE = os.path.expanduser("~/.config/p4disp/restore_token")

SOURCE_MONITOR, SOURCE_WINDOW = 1, 2
CURSOR_EMBEDDED = 2
PERSIST_UNTIL_REVOKED = 2


class Portal:
    """Minimal synchronous wrapper over the ScreenCast portal's request/response dance."""

    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")
        self.loop = GLib.MainLoop()

    def _token(self):
        return f"p4disp{random.randrange(1 << 30)}"

    def _request(self, method, args_fmt, args, options):
        """Call a portal method that answers via a Request object's Response signal."""
        token = self._token()
        options["handle_token"] = GLib.Variant("s", token)
        path = f"/org/freedesktop/portal/desktop/request/{self.sender}/{token}"
        result = {}

        def on_response(_conn, _sender, _path, _iface, _signal, params):
            code, results = params.unpack()
            result["code"], result["results"] = code, results
            self.loop.quit()

        # Subscribe before calling, or a fast portal can answer before we listen.
        sub = self.bus.signal_subscribe(PORTAL, "org.freedesktop.portal.Request", "Response",
                                        path, None, Gio.DBusSignalFlags.NO_MATCH_RULE, on_response)
        try:
            self.bus.call_sync(PORTAL, PORTAL_PATH, SCREENCAST, method,
                               GLib.Variant(f"({args_fmt}a{{sv}})", (*args, options)),
                               None, Gio.DBusCallFlags.NONE, -1, None)
            self.loop.run()
        finally:
            self.bus.signal_unsubscribe(sub)

        if result["code"] != 0:
            reason = {1: "cancelled by user", 2: "failed"}.get(result["code"], str(result["code"]))
            raise RuntimeError(f"portal {method}: {reason}")
        return result["results"]

    def _prop(self, name):
        v = self.bus.call_sync(PORTAL, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
                               GLib.Variant("(ss)", (SCREENCAST, name)), GLib.VariantType("(v)"),
                               Gio.DBusCallFlags.NONE, -1, None)
        return v.unpack()[0]

    def start(self, types, restore_token=None):
        """Returns (pipewire_fd, node_id, stream_size, new_restore_token)."""
        res = self._request("CreateSession", "", (),
                            {"session_handle_token": GLib.Variant("s", self._token())})
        session = res["session_handle"]

        opts = {
            "types": GLib.Variant("u", types),
            "multiple": GLib.Variant("b", False),
            "persist_mode": GLib.Variant("u", PERSIST_UNTIL_REVOKED),
        }
        if self._prop("AvailableCursorModes") & CURSOR_EMBEDDED:
            opts["cursor_mode"] = GLib.Variant("u", CURSOR_EMBEDDED)
        if restore_token:
            opts["restore_token"] = GLib.Variant("s", restore_token)
        self._request("SelectSources", "o", (session,), opts)

        res = self._request("Start", "os", (session, ""), {})
        streams = res.get("streams") or []
        if not streams:
            raise RuntimeError("portal returned no streams")
        node_id, props = streams[0]

        reply, fds = self.bus.call_with_unix_fd_list_sync(
            PORTAL, PORTAL_PATH, SCREENCAST, "OpenPipeWireRemote",
            GLib.Variant("(oa{sv})", (session, {})), GLib.VariantType("(h)"),
            Gio.DBusCallFlags.NONE, -1, None, None)
        fd = fds.get(reply.unpack()[0])
        return fd, node_id, props.get("size"), res.get("restore_token")


def load_token():
    try:
        with open(TOKEN_FILE) as f:
            return f.read().strip() or None
    except OSError:
        return None


def save_token(token):
    if token:
        os.makedirs(os.path.dirname(TOKEN_FILE), exist_ok=True)
        with open(TOKEN_FILE, "w") as f:
            f.write(token)


def build_pipeline(fd, node_id, rotate, quality):
    # videoflip: counterclockwise == PIL rotate(90), which test_card.py uses.
    flip = {0: "none", 90: "counterclockwise", 180: "rotate-180", 270: "clockwise"}[rotate]
    land_w, land_h = (PANEL_H, PANEL_W) if rotate in (90, 270) else (PANEL_W, PANEL_H)
    desc = (
        f"pipewiresrc fd={fd} path={node_id} do-timestamp=true keepalive-time=1000 always-copy=true "
        f"! videoconvert ! videoscale add-borders=true "
        f"! video/x-raw,width={land_w},height={land_h},pixel-aspect-ratio=1/1 "
        f"! videoflip method={flip} ! videoconvert "
        # I420 in => 4:2:0 JPEG out. The board's decoder hangs on 4:2:2.
        f"! video/x-raw,format=I420,width={PANEL_W},height={PANEL_H} "
        f"! jpegenc quality={quality} "
        f"! appsink name=sink max-buffers=1 drop=true sync=false emit-signals=false"
    )
    return Gst.parse_launch(desc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pick", action="store_true", help="ignore the saved choice and show the picker")
    ap.add_argument("--window", action="store_true", help="offer windows as well as monitors")
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--rotate", type=int, default=90, choices=(0, 90, 180, 270))
    ap.add_argument("--max-fps", type=float, default=60)
    args = ap.parse_args()

    Gst.init(None)
    disp = P4Display()
    print(f"display connected ({'high' if disp.high_speed else 'FULL - slow'} speed)")

    types = SOURCE_MONITOR | (SOURCE_WINDOW if args.window else 0)
    token = None if args.pick else load_token()
    portal = Portal()
    try:
        fd, node, size, new_token = portal.start(types, token)
    except RuntimeError as e:
        if token:  # stale token (monitor gone, permission revoked) - ask again
            print(f"{e}; asking again")
            fd, node, size, new_token = portal.start(types, None)
        else:
            sys.exit(str(e))
    save_token(new_token)
    print(f"sharing PipeWire node {node}, source size {size}")

    pipe = build_pipeline(fd, node, args.rotate, args.quality)
    sink = pipe.get_by_name("sink")
    bus = pipe.get_bus()
    pipe.set_state(Gst.State.PLAYING)

    min_period = 1.0 / args.max_fps
    last_send = 0.0
    n = total = 0
    t_report = time.perf_counter()
    try:
        while True:
            msg = bus.pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
            if msg:
                if msg.type == Gst.MessageType.ERROR:
                    err, dbg = msg.parse_error()
                    sys.exit(f"pipeline error: {err.message}\n{dbg}")
                sys.exit("stream ended (share stopped?)")

            sample = sink.emit("try-pull-sample", 200 * Gst.MSECOND)
            now = time.perf_counter()
            if sample is not None and now - last_send >= min_period:
                buf = sample.get_buffer()
                ok, info = buf.map(Gst.MapFlags.READ)
                if ok:
                    try:
                        disp.send_jpeg(bytes(info.data))
                        total += info.size
                        n += 1
                    finally:
                        buf.unmap(info)
                last_send = now

            if now - t_report >= 2:
                s = disp.stats()
                el = now - t_report
                kb = total / n / 1024 if n else 0
                print(f"{n / el:5.1f} fps  {kb:5.0f} KB/frame | board shown {s.get('frames_shown')} "
                      f"stale {s['frames_stale']} decode {s.get('decode_us_last', 0) / 1000:.1f} ms "
                      f"fail {s.get('decode_fail')}", flush=True)
                n = total = 0
                t_report = now
    except KeyboardInterrupt:
        pass
    finally:
        pipe.set_state(Gst.State.NULL)


if __name__ == "__main__":
    main()
